#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <wafel/dynamic.h>
#include <wafel/ios_dynamic.h>
#include <wafel/utils.h>
#include <wafel/patch.h>
#include <wafel/ios/svc.h>
#include <wafel/trampoline.h>
#include "mbr.h"

#define SECTOR_SIZE 512
#define LOCAL_HEAP_ID 0xCAFE
#define DEVTYPE_USB 17

#define SERVER_HANDLE_LEN 0xb5
#define SERVER_HANDLE_SZ (SERVER_HANDLE_LEN * sizeof(int))

#define LD_DWORD(ptr)       (u32)(((u32)*((u8*)(ptr)+3)<<24)|((u32)*((u8*)(ptr)+2)<<16)|((u16)*((u8*)(ptr)+1)<<8)|*(u8*)(ptr))

static int (*FSSAL_attach_device)(int*) = (void*)0x10733aa4;

#define FIRST_HANDLE ((int*)0x11c39e78)
#define HANDLE_END ((int*)0x11c3a420)

int extra_server_handle[SERVER_HANDLE_LEN]; // = HANDLE_END-SERVER_HANDLE_LEN;

static u32 sdusb_offset = 0xFFFFFFF;
static u32 sdusb_size = 0xFFFFFFFF;

static int (*real_read)(int*, u32, u32, u32, u32, void*, void*, void*) = (void*)0x107bddd0;
static int (*real_write)(int*, u32, u32, u32, u32, void*, void*, void*) = (void*)0x107bdd60;


static int read_wrapper(void *device_handle, u32 lba_hi, u32 lba, u32 blkCount, u32 blockSize, void *buf, void *cb, void* cb_ctx){
    return real_read(device_handle, lba_hi, lba + sdusb_offset, blkCount, blockSize, buf, cb, cb_ctx);
}

static int write_wrapper(void *device_handle, u32 lba_hi, u32 lba, u32 blkCount, u32 blockSize, void *buf, void *cb, void* cb_ctx){
    return real_write(device_handle, lba_hi, lba + sdusb_offset, blkCount, blockSize, buf, cb, cb_ctx);
}

static partition_entry* find_usb_partition(mbr_sector* mbr){
    for (size_t i = 1; i < MBR_MAX_PARTITIONS; i++){
        if(mbr->partition[i].type == MBR_PARTITION_TYPE_MLC_NOSCFM){
            return mbr->partition+i;
        }
    }
    return NULL;
}

struct cb_ctx {
    int semaphore;
    int res;
} typedef cb_ctx;

static void read_callback(int res, cb_ctx *ctx){
    debug_printf("In read_callback(%d,%p)\n", res, ctx);
    ctx->res = res;
    iosSignalSemaphore(ctx->semaphore);
}

void hook_register_sd(trampoline_state *state){
    int *server_handle = (int*)state->r[0] -3;
    debug_printf("SDUSB: org server_handle: %p\n", server_handle);
    real_read = (void*)server_handle[0x76];
    real_write = (void*)server_handle[0x78];
    u8 *buf = iosAllocAligned(LOCAL_HEAP_ID, SECTOR_SIZE, 0x40);
    if(!buf){
        debug_printf("SDUSB: Failed to allocate IO buf\n");
        return;
    }

    cb_ctx ctx = {iosCreateSemaphore(1,0)};
    if(ctx.semaphore < 0){
        debug_printf("SDUSB: Error creating Semaphore: 0x%X\n", ctx.semaphore);
    }

    debug_printf("Calling sdio_read at %p\n", real_read);
    int res = real_read(server_handle, 0, 0, 1, SECTOR_SIZE, buf, read_callback, &ctx);
    debug_printf("sdio_read returned: %u\n", res);

    debug_printf("SDUSB: Waiting for semaphore\n");
    iosWaitSemaphore(ctx.semaphore, 0);

    iosDestroySemaphore(ctx.semaphore);

    partition_entry *part = find_usb_partition((mbr_sector*)buf);

    if(!part){
        debug_printf("SDUSB: USB partition not found!!!\n");
        iosFree(LOCAL_HEAP_ID, buf); // also frees part
        return;
    }
    
    sdusb_offset = LD_DWORD(part->lba_start);
    sdusb_size = LD_DWORD(part->lba_length);

    debug_printf("SDUSB: raw part offset: %02X %02X %02X %02X, size: %02X %02X %02X %02X\n", 
            part->lba_start[0], part->lba_start[1], part->lba_start[2], part->lba_start[3],
                part->lba_length[0], part->lba_length[1],part->lba_length[2],part->lba_length[3]);

    iosFree(LOCAL_HEAP_ID, buf); // also frees part

    debug_printf("SDUSB: USB partition found %p: offset: %u, size: %u\n", part, sdusb_offset, sdusb_size);

    //print_handles();

    // the virtual USB device has to use the original slot, so the sd goes to the extra slot
    memcpy(extra_server_handle, server_handle, SERVER_HANDLE_SZ);
    res = FSSAL_attach_device(extra_server_handle+3);
    extra_server_handle[0x82] = res;

    int *sdusb_server_handle = server_handle;

    sdusb_server_handle[0x3] = (int) sdusb_server_handle;
    sdusb_server_handle[0x76] = (int)read_wrapper;
    sdusb_server_handle[0x78] = (int)write_wrapper;
    sdusb_server_handle[0x5] = DEVTYPE_USB;
    sdusb_server_handle[0xa] = sdusb_size -1;
    sdusb_server_handle[0x1] = sdusb_server_handle[0xe] = sdusb_size;

    //sdusb_attach_device_handle[0x83] = 0xFF;

    //res = FSSAL_attach_device(sdusb_attach_device_handle+3);
    //sdusb_attach_device_handle[0x82] = res;

    debug_printf("SDUSB: Attached pseudo USB device. res: 0x%X\n", res);
}

void crypto_hook(trampoline_state* state){
    if(state->r[5] == sdusb_size){
        //debug_printf("SDUSB: cryptohook detected USB partition true lr: %p\n", state->lr);
        state->r[0] = 0xDEADBEEF;
    }
}

// This fn runs before everything else in kernel mode.
// It should be used to do extremely early patches
// (ie to BSP and kernel, which launches before MCP)
// It jumps to the real IOS kernel entry on exit.
__attribute__((target("arm")))
void kern_main()
{
    // Make sure relocs worked fine and mappings are good
    debug_printf("we in here trampoline demo plugin kern %p\n", kern_main);

    debug_printf("init_linking symbol at: %08x\n", wafel_find_symbol("init_linking"));

    trampoline_hook_before(0x107bd9a4, hook_register_sd);
    trampoline_hook_before(0x10740f48, crypto_hook); // hook decrypt call
    trampoline_hook_before(0x10740fe8, crypto_hook); // hook encrypt call
}

// This fn runs before MCP's main thread, and can be used
// to perform late patches and spawn threads under MCP.
// It must return.
void mcp_main()
{

}
