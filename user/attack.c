#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

#define MSG "Here it is: "

int
main(int argc, char *argv[])
{
  int total_size = 32 * PGSIZE;
  char *mem = sbrk(total_size);
  int msg_len = strlen(MSG);
  if (mem == (char*)-1) {
    exit(1);
  }

  for (int i = 0; i < total_size - msg_len; i++) {
    int match = 1;
    for (int j = 0; j < msg_len; j++) {
      if (mem[i + j] != MSG[j]) {
        match = 0;
        break;
      }
    }

    if (match && mem[i + msg_len] != '\0') {
      printf("%s\n", mem + i + msg_len);
      exit(0);
    }
  }

  exit(1);
}
