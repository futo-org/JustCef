#include "main.h"

#if !defined(OS_WIN)
int main(int argc, char* argv[]) {
  return shared::main(argc, argv);
}
#endif
