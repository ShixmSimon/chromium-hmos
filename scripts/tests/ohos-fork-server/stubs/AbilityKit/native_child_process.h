// Only the public ABI structs used by the loader. OHOS builds use SDK headers.
#pragma once
struct NativeChildProcess_Fd {
  char* fdName;
  int fd;
  NativeChildProcess_Fd* next;
};
struct NativeChildProcess_FdList { NativeChildProcess_Fd* head; };
struct NativeChildProcess_Args {
  char* entryParams;
  NativeChildProcess_FdList fdList;
};
