#include <assert.h>

#include "ps4.h"

const uint8_t payload_data_const[] =
{
#include "payload_data.inc"
};

uint64_t __readmsr(unsigned long __register)
{
  unsigned long __edx;
  unsigned long __eax;
  __asm__ ("rdmsr" : "=d"(__edx), "=a"(__eax) : "c"(__register));
  return (((uint64_t)__edx) << 32) | (uint64_t)__eax;
}

#define X86_CR0_WP (1 << 16)

static inline __attribute__((always_inline)) uint64_t readCr0(void)
{
  uint64_t cr0;
  __asm__ volatile ("movq %0, %%cr0" : "=r" (cr0) : : "memory");
  return cr0;
}

static inline __attribute__((always_inline)) void writeCr0(uint64_t cr0)
{
  __asm__ volatile("movq %%cr0, %0" : : "r" (cr0) : "memory");
}

struct payload_info
{
  uint8_t* buffer;
  size_t size;
};

struct syscall_install_payload_args
{
  void* syscall_handler;
  struct payload_info* payload_info;
};

struct real_info
{
  const size_t kernel_offset;
  const size_t payload_offset;
};

struct cave_info
{
  const size_t kernel_offset;
  const size_t payload_offset;
};

struct disp_info
{
  const size_t call_offset;
  const size_t cave_offset;
};

struct payload_header
{
  uint64_t signature;
  size_t real_info_offset;
  size_t cave_info_offset;
  size_t disp_info_offset;
  size_t entrypoint_offset;
};

int syscall_install_payload(void* td, struct syscall_install_payload_args* args)
{
  uint64_t cr0;
  typedef uint64_t vm_offset_t;
  typedef uint64_t vm_size_t;
  typedef void* vm_map_t;

  void* (*kernel_memcpy)(void* dst, const void* src, size_t len);
  void (*kernel_printf)(const char* fmt, ...);
  vm_offset_t (*kmem_alloc)(vm_map_t map, vm_size_t size);

  uint8_t* kernel_base = (uint8_t*)(__readmsr(0xC0000082) - 0x30EB30);

  *(void**)(&kernel_printf) = &kernel_base[0x347580];
  *(void**)(&kernel_memcpy) = &kernel_base[0x286CF0];
  *(void**)(&kmem_alloc) = &kernel_base[0x369500];
  vm_map_t kernel_map = *(void**)&kernel_base[0x1FE71B8];

  kernel_printf("\n\n\n\npayload_installer: starting\n");
  kernel_printf("payload_installer: kernel base=%lx\n", kernel_base);

  if (!args->payload_info)
  {
    kernel_printf("payload_installer: bad payload info\n");
    return -1;
  }

  uint8_t* payload_data = args->payload_info->buffer;
  size_t payload_size = args->payload_info->size;
  struct payload_header* payload_header = (struct payload_header*)payload_data;

  if (!payload_data ||
      payload_size < sizeof(payload_header) ||
      payload_header->signature != 0x5041594C4F414432ull)
  {
    kernel_printf("payload_installer: bad payload data\n");
    return -2;
  }

  int desired_size = (payload_size + 0x3FFFull) & ~0x3FFFull; // align size

  // TODO(idc): clone kmem_alloc instead of patching directly
  cr0 = readCr0();
  writeCr0(cr0 & ~X86_CR0_WP);
  kernel_base[0x36958D] = 7;
  kernel_base[0x3695A5] = 7;
  writeCr0(cr0);

  kernel_printf("payload_installer: kmem_alloc\n");
  uint8_t* payload_buffer = (uint8_t*)kmem_alloc(kernel_map, desired_size);
  if (!payload_buffer)
  {
    kernel_printf("payload_installer: kmem_alloc failed\n");
    return -3;
  }

  // TODO(idc): clone kmem_alloc instead of patching directly
  cr0 = readCr0();
  writeCr0(cr0 & ~X86_CR0_WP);
  kernel_base[0x36958D] = 3;
  kernel_base[0x3695A5] = 3;
  writeCr0(cr0);

  kernel_printf("payload_installer: installing...\n");
  kernel_printf("payload_installer: target=%lx\n", payload_buffer);
  kernel_printf("payload_installer: payload=%lx,%lu\n",
    payload_data, payload_size);

  kernel_printf("payload_installer: memcpy\n");
  kernel_memcpy((void*)payload_buffer, payload_data, payload_size);

  kernel_printf("payload_installer: patching payload pointers\n");
  if (payload_header->real_info_offset != 0 &&
    payload_header->real_info_offset + sizeof(struct real_info) <= payload_size)
  {
    struct real_info* real_info =
      (struct real_info*)(&payload_data[payload_header->real_info_offset]);
    for (
      ; real_info->payload_offset != 0 && real_info->kernel_offset != 0
      ; ++real_info)
    {
      uint64_t* payload_target =
        (uint64_t*)(&payload_buffer[real_info->payload_offset]);
      void* kernel_target = &kernel_base[real_info->kernel_offset];
      *payload_target = (uint64_t)kernel_target;
      kernel_printf("  %x(%lx) = %x(%lx)\n",
        real_info->payload_offset, payload_target,
        real_info->kernel_offset, kernel_target);
    }
  }

  kernel_printf("payload_installer: patching caves\n");
  if (payload_header->cave_info_offset != 0 &&
    payload_header->cave_info_offset + sizeof(struct cave_info) <= payload_size)
  {
    struct cave_info* cave_info =
      (struct cave_info*)(&payload_data[payload_header->cave_info_offset]);
    for (
      ; cave_info->payload_offset != 0 && cave_info->kernel_offset != 0
      ; ++cave_info)
    {
      uint8_t* kernel_target = &kernel_base[cave_info->kernel_offset];
      void* payload_target = &payload_buffer[cave_info->payload_offset];
      kernel_printf("  %lx(%lx) : %lx(%lx)\n",
        cave_info->kernel_offset, kernel_target,
        cave_info->payload_offset, payload_target);

#pragma pack(push,1)
      struct
      {
        uint8_t op[2];
        int32_t disp;
        uint64_t address;
      }
      jmp;
#pragma pack(pop)
      jmp.op[0] = 0xFF;
      jmp.op[1] = 0x25;
      jmp.disp = 0;
      jmp.address = (uint64_t)payload_target;
      cr0 = readCr0();
      writeCr0(cr0 & ~X86_CR0_WP);
      kernel_memcpy(kernel_target, &jmp, sizeof(jmp));
      writeCr0(cr0);
    }
  }

  kernel_printf("payload_installer: patching calls\n");
  if (payload_header->disp_info_offset != 0 &&
    payload_header->disp_info_offset + sizeof(struct disp_info) <= payload_size)
  {
    struct disp_info* disp_info =
      (struct disp_info*)(&payload_data[payload_header->disp_info_offset]);
    for (
      ; disp_info->call_offset != 0 && disp_info->cave_offset != 0
      ; ++disp_info)
    {
      uint8_t* cave_target = &kernel_base[disp_info->cave_offset];
      uint8_t* call_target = &kernel_base[disp_info->call_offset];

      int32_t new_disp = (int32_t)(cave_target - &call_target[5]);

      kernel_printf("  %lx(%lx)\n",
        disp_info->call_offset + 1, &call_target[1]);
      kernel_printf("    %lx(%lx) -> %lx(%lx) = %d\n",
        disp_info->call_offset + 5, &call_target[5],
        disp_info->cave_offset, cave_target,
        new_disp);

      cr0 = readCr0();
      writeCr0(cr0 & ~X86_CR0_WP);
      *((int32_t*)&call_target[1]) = new_disp;
      writeCr0(cr0);
    }
  }

  if (payload_header->entrypoint_offset != 0 &&
    payload_header->entrypoint_offset < payload_size)
  {
    kernel_printf("payload_installer: entrypoint\n");
    void (*payload_entrypoint)();
    *((void**)&payload_entrypoint) =
      (void*)(&payload_buffer[payload_header->entrypoint_offset]);
    payload_entrypoint();
  }

  kernel_printf("payload_installer: done\n");
  return 0;
}

int _main(void)
{
  uint8_t* payload_data = (uint8_t*)(&payload_data_const[0]);
  size_t payload_size = sizeof(payload_data_const);

  initKernel();
  struct payload_info payload_info;
  payload_info.buffer = payload_data;
  payload_info.size = payload_size;
  errno = 0;
  int result = kexec(&syscall_install_payload, &payload_info);
  return !result ? 0 : errno;
}
