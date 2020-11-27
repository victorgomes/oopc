#include <lib/fdio/spawn.h>
#include <zircon/types.h>
#include <zircon/syscalls.h>
#include <zircon/process.h>
#include <zircon/processargs.h>

#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

void jit_process(zx_handle_t channel) {
  // Wait indefintely the channel to read vmo.
  assert(zx_object_wait_one(channel, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
    ZX_TIME_INFINITE, nullptr) == ZX_OK);

  // Read vmo.
  zx_handle_t vmo;
  zx_channel_read(channel, 0, nullptr, &vmo, 0, 1, nullptr, nullptr);

  // We could create a new vmo and validate the input.

  // Eventually set as executable.
  assert(zx_vmo_replace_as_executable(vmo, ZX_HANDLE_INVALID, &vmo) == ZX_OK);

  // Send the VMO back, now with executable right, but not writable.
  assert(zx_object_wait_one(channel, ZX_CHANNEL_WRITABLE | ZX_CHANNEL_PEER_CLOSED,
    ZX_TIME_INFINITE, nullptr) == ZX_OK);
  zx_channel_write(channel, 0, nullptr, 0, &vmo, 1);
}

void main_process(zx_handle_t jit, zx_handle_t channel) {
  // Return 42
  uint8_t code[] = {
    0x55,                         // push   %rbp
    0x48, 0x89, 0xe5,             // mov    %rsp,%rbp
    0xb8, 0x2a, 0x00, 0x00, 0x00, // mov    $0x2a,%eax
    0x5d,                   	    // pop    %rbp
    0xc3                   	      // retq   
  };
  size_t code_size = sizeof(code)/sizeof(uint8_t);

  // Create VMO and copy code into it.
  zx_handle_t vmo;
  assert(zx_vmo_create(code_size, 0, &vmo) == ZX_OK);
  assert(zx_vmo_write(vmo, code, 0, code_size) == ZX_OK);

  // Send VMO to JIT process.
  assert(zx_object_wait_one(channel, ZX_CHANNEL_WRITABLE| ZX_CHANNEL_PEER_CLOSED,
    ZX_TIME_INFINITE, nullptr) == ZX_OK);
  zx_channel_write(channel, 0, nullptr, 0, &vmo, 1);

  // VMO handle is closed for this process. We cannot read, write nor execute.

  // Read handle from the JIT process. We receive it with executable right.
  // We cannot write to it anymore.
  assert(zx_object_wait_one(channel, ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
    ZX_TIME_INFINITE, nullptr) == ZX_OK);
  zx_channel_read(channel, 0, nullptr, &vmo, 0, 1, nullptr, nullptr);

  // Map to the local virtual address.
  zx_vaddr_t mapped_code;
  size_t vmo_size; // Page aligned.
  assert(zx_vmo_get_size(vmo, &vmo_size) == ZX_OK);
  assert(zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_EXECUTE, 0, vmo, 0, vmo_size, &mapped_code) == ZX_OK);

  // Run JIT
  int (*func)() = (int (*)()) mapped_code;
  printf("Return JIT code: %d\n", func());
}

zx_handle_t spawn_oopc(zx_handle_t channel) {
  zx_handle_t jit;
  const char *path = "/pkg/oopc_bin";
  const char *argv[] = { path, "--jit", nullptr};
  char err_msg_out[FDIO_SPAWN_ERR_MSG_MAX_LENGTH];
  fdio_spawn_action_t actions[] = {
    {
      .action =  FDIO_SPAWN_ACTION_ADD_HANDLE,
      .h = {.id = PA_USER0, .handle = channel}
    },
  };
  zx_status_t status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL,
    path, argv, nullptr, sizeof(actions) / sizeof(fdio_spawn_action_t), actions,
    &jit, err_msg_out);
  if(status != ZX_OK) {
    printf("error: %s\n", err_msg_out);
    assert(false);
  }
  return jit;
}

int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "--jit") == 0) {
    // Get channel from parent
    zx_handle_t channel = zx_take_startup_handle(PA_HND(PA_USER0, 0));
    // Run JIT process
    jit_process(channel);
    return 0;
  }
  // Create communication channels
  zx_handle_t ch_jit, ch_main;
  assert(zx_channel_create(0, &ch_main, &ch_jit) == ZX_OK);
  // Spawn child
  zx_handle_t jit = spawn_oopc(ch_jit);
  // Run main process
  main_process(jit, ch_main);
}
