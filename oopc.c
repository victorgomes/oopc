// cc oopc.c -o oopc -Wall -lrt 

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/msg.h>
#include <sys/ipc.h>

struct message {
  char name[8];
  size_t size;
};

void die(const char *msg) {
  perror(msg);
  exit(-1);
}

size_t estimate_memory_size(size_t machine_code_size) {
  size_t page_size_multiple = sysconf(_SC_PAGE_SIZE); // Get the machine page size
  size_t factor = 1, required_memory_size;

  for(;;) {
      required_memory_size = factor * page_size_multiple;
      if(machine_code_size <= required_memory_size) break;
      factor++;
  }
  return required_memory_size;
}

void oopc_process() {
  printf("Running OOPC process...\n");

  // Create request and answer queues.
  int req_queue = msgget(ftok("oopc", 'r'), 0666 | IPC_CREAT);
  int ans_queue = msgget(ftok("oopc", 'a'), 0666 | IPC_CREAT);

  // Receive request.
  struct message req;
  msgrcv(req_queue, &req, sizeof(req), 0, 0);
  printf("OOPC: Request received id=%d, name=%.8s, size=%ld\n",
         req_queue, req.name, req.size);

  // Get code in shared memory.
  int fd_req = shm_open("code", O_RDWR, 0x600);
  if (fd_req < 0) die("OOPC: Can't get file descriptor");
  void *codeptr = (uint8_t*) mmap(NULL, req.size, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd_req, 0);
  if ((void*) -1 == codeptr) die("OOPC: Can't access segment...");

  // Get the required memory size for mmap
  size_t required_memory_size = estimate_memory_size(req.size);

  // Create answer.
  struct message ans;
  strcpy(ans.name, "moopc");
  ans.size = required_memory_size;

  // Create shared memory to allocate JIT code.
  int fd_ans = shm_open(ans.name, O_RDWR | O_CREAT, 0600);
  if (fd_ans < 0) die("OOPC: Can't get file descriptor");
  ftruncate(fd_ans, required_memory_size); // Enough space to cover a page.
  void *ansptr = mmap(NULL, req.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                      fd_ans, 0);
  if ((void*) -1 == ansptr) die("OOPC: Can't access segment...");

  // Here we would also validate the input.

  // Copy code to new buffer.
  memcpy(ansptr, codeptr, req.size);

  // Send answer.
  printf("OOPC: Answer sent id=%d, name=%.8s, size=%ld\n", ans_queue, ans.name,
         ans.size);
  msgsnd(ans_queue, &ans, sizeof(ans), 0);

  // Create time for the main process to access the answer, before we clean up.
  sleep(1);

  // Cleanup
  printf("OOPC: Cleaning up\n");
  msgctl(req_queue, IPC_RMID, NULL);
  msgctl(ans_queue, IPC_RMID, NULL);
  munmap(codeptr, req.size);
  munmap(ansptr, ans.size);
  close(fd_req);
  close(fd_ans);
  unlink(req.name);
  unlink(ans.name);
}


void main_process() {
  printf("Running main process...\n");

  uint8_t code[] = {
      0x48, 0xc7, 0xc0, 0x01, 0x00, 0x00, 0x00, // Store the "write" system call
      0x48, 0xc7, 0xc7, 0x01, 0x00, 0x00, 0x00, // Store stdin file descriptor 0x01
      0x48, 0x8d, 0x35, 0x0a, 0x00, 0x00, 0x00, // Store the location of the string
      0x48, 0xc7, 0xc2, 0x0e, 0x00, 0x00, 0x00, // Store the length of the string
      0x0f, 0x05,                               // Execute the system call
      0xc3,                                     // return instruction
      0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x2c, 0x20, // 'Hello, '
      0x57, 0x6f, 0x72, 0x6c, 0x64, 0x21, 0x0a  // 'World!\n'
  };
  size_t code_size = sizeof(code)/sizeof(uint8_t);

  // Create request and answer queues.
  int req_queue = msgget(ftok("oopc", 'r'), 0666 | IPC_CREAT);
  int ans_queue = msgget(ftok("oopc", 'a'), 0666 | IPC_CREAT);

  // Create request message
  struct message req;
  strcpy(req.name, "code");
  req.size = code_size;

  // Create shared memory to add code.
  int fd_req = shm_open(req.name, O_RDWR | O_CREAT, 0600);
  if (fd_req < 0) die("Main: Can't open shared mem...");
  ftruncate(fd_req, code_size); // Enough space to cover a page.
  void *codeptr = mmap(NULL, code_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                       fd_req, 0);
  if ((void*) -1 == codeptr) die("Main: Can't access segment...");

  // Copy to shared buffer.
  memcpy(codeptr, code, code_size);

  // Send request.
  printf("Main: Request sent id=%d, name=%.8s, size=%ld\n", req_queue, req.name,
         req.size);
  msgsnd(req_queue, &req, sizeof(req), 0);

  // Receive answer
  struct message ans;
  msgrcv(ans_queue, &ans, sizeof(ans), 0, 0);
  printf("Main: Request received id=%d, name=%.8s, size=%ld\n", ans_queue,
         ans.name, ans.size);

  // Get shared memory to execute the code.
  int fd_ans = shm_open(ans.name, O_RDWR, 0600);
  if (fd_ans < 0) die("Main: Can't get file descriptor");
  void *ansptr = mmap(NULL, req.size, PROT_READ | PROT_EXEC, MAP_SHARED,
                      fd_ans, 0);
  if ((void*) -1 == ansptr) die("Main: Can't access segment...");

  // Run JIT code
  void (*func)();
  func = (void (*)()) ansptr;
  func();

  // Cleanup
  printf("Main: Cleaning up\n");
  msgctl(req_queue, IPC_RMID, NULL);
  msgctl(ans_queue, IPC_RMID, NULL);
  munmap(codeptr, req.size);
  munmap(ansptr, ans.size);
  close(fd_req);
  close(fd_ans);
  unlink(req.name);
  unlink(ans.name);
}

int main() {
  if (fork() == 0) oopc_process();
  else main_process();
}
