/*-
 * Copyright (c) 2016 Thomas Haggett
 * Copyright (c) 2016 Pavel Borzenkov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THOMAS HAGGETT ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL NETAPP, INC OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/mman.h>

#include <xhyve/firmware/multiboot.h>
#include <xhyve/vmm/vmm_api.h>

#define MULTIBOOT_MAGIC 0x1BADB002
#define MULTIBOOT_SEARCH_END 0x2000

struct multiboot_load_header {
    uint32_t header_addr;
    uint32_t load_addr;
    uint32_t load_end_addr;
    uint32_t bss_end_addr;
    uint32_t entry_addr;
};

struct multiboot_video_header {
    uint32_t mode_type;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
};

struct multiboot_header {
    struct {
        uint32_t magic;
        uint32_t flags;
        uint32_t checksum;
    } hdr;
    struct multiboot_load_header lhdr;
    struct multiboot_video_header vhdr;
};

struct multiboot_info  {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline_addr;
    uint32_t mods_count;
    uint32_t mods_addr;
};

struct multiboot_module_entry {
    uint32_t addr_start;
    uint32_t addr_end;
    uint32_t cmdline;
    uint32_t pad;
};

static struct multiboot_config {
    char* kernel_path;
    char* module_list;
    char* kernel_append;
} config;

struct boot_config {
    void *mapping;
    size_t file_size;

    uintptr_t header;
    uintptr_t guest_mem_base;
    uintptr_t guest_mem_size;
    uint32_t load_alignment;
    uint32_t provide_mem_headers;

    struct multiboot_load_header kernel_load_data;
    uint32_t pad;
};

struct elf_ehdr {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_hsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct elf32_shdr {
    uint32_t    sh_name;
    uint32_t    sh_type;
    uint32_t    sh_flags;
    uint32_t    sh_addr;
    uint32_t    sh_offset;
    uint32_t    sh_size;
    uint32_t    sh_link;
    uint32_t    sh_info;
    uint32_t    sh_addralign;
    uint32_t    sh_entsize;
};

#define EM_X86_64 62

struct elf_phdr {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
};

#define PT_LOAD 1

#define PF_X 0x1

#define ROUND_UP(a, b) (((a) + (b) - 1) / (b) * (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

//
// called by xhyve to pass in the firmware arguments
//
void
multiboot_init(char *kernel_path, char *module_list, char *kernel_append)
{
    config.kernel_path = kernel_path;
    config.module_list = module_list;
    config.kernel_append = kernel_append;
}

static int
multiboot_parse_elf(struct boot_config *bc)
{
    struct elf_ehdr *ehdr = bc->mapping;
    struct elf_phdr *phdr;
    uint32_t low = (uint32_t)-1, high = 0, memsize, addr, entry;
    int i;

    if (ehdr->e_ident[0] != 0x7f ||
            ehdr->e_ident[1] != 'E' ||
            ehdr->e_ident[2] != 'L' ||
            ehdr->e_ident[3] != 'F') {
        fprintf(stderr, "multiboot: invalid ELF magic\n");
        return -1;
    }
    if (ehdr->e_machine == EM_X86_64) {
        fprintf(stderr, "multiboot: 64-bit ELFs are not supported\n");
        return -1;
    }

    entry = ehdr->e_entry;

    phdr = (struct elf_phdr *)((uintptr_t)bc->mapping + ehdr->e_phoff);
    for (i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD)
            continue;
        memsize = phdr[i].p_filesz;
        addr = phdr[i].p_paddr;

        if (phdr[i].p_flags & PF_X &&
                phdr[i].p_vaddr != phdr[i].p_paddr &&
                entry >= phdr[i].p_vaddr &&
                entry < phdr[i].p_vaddr + phdr[i].p_filesz)
            entry = entry - phdr[i].p_vaddr + phdr[i].p_paddr;

        if (addr < low)
            low = addr;
        if (addr + memsize > high)
            high = addr + memsize;
    }

    if (low == (uint32_t)-1 || high == 0) {
        fprintf(stderr, "multiboot: failed to parse ELF file\n");
        return -1;
    }

    bc->kernel_load_data.header_addr =
        (uint32_t)(low + (bc->header - (uintptr_t)bc->mapping));
    bc->kernel_load_data.load_addr = low;
    bc->kernel_load_data.load_end_addr = high;
    bc->kernel_load_data.bss_end_addr = 0; // TODO
    bc->kernel_load_data.entry_addr = entry;

    return 0;
}

// scans the configured kernel for it's multiboot header.
// returns -1 if no multiboot header is found, 0 if one is.
static int
multiboot_find_header(struct boot_config* bc)
{
    struct multiboot_header *header = NULL;
    uintptr_t ptr = (uintptr_t)bc->mapping;
    uintptr_t sz = MIN(bc->file_size, MULTIBOOT_SEARCH_END);
    uintptr_t end = ptr + sz - 48; /* 48 - size of multiboot header */
    int found = 0, ret = -1;

    for (; ptr < end; ptr += 4) {
        header = (struct multiboot_header *)ptr;

        if (header->hdr.magic != MULTIBOOT_MAGIC)
               continue;
        if (header->hdr.checksum + header->hdr.flags + header->hdr.magic != 0)
            continue;

        found = 1;
        break;
    }
    if (!found)
        return ret;
    bc->header = (uintptr_t)header;

    // are there any mandatory flags that we don't support? (any other than 0 and 1 set)
    uint16_t supported_mandatory = ((1 << 1) | (1 << 0));
    if (((header->hdr.flags & ~supported_mandatory) & 0xFFFF) != 0x0) {
        fprintf(stderr, "multiboot: header has unsupported mandatory "
                "flags (0x%x), bailing.\n", header->hdr.flags & 0xFFFF);
        return ret;
    }

    // at this point, we need to check the flags and pull in the additional sections
    if (header->hdr.flags & (1 << 0))
        bc->load_alignment = 4096;

    if (header->hdr.flags & (1 << 1))
        bc->provide_mem_headers = 1;

    if (header->hdr.flags & (1<<16)) {
        memcpy(&bc->kernel_load_data, &header->lhdr, sizeof(header->lhdr));
        ret = 0;
    } else {
        fprintf(stderr, "Parse elf\n");
        ret = multiboot_parse_elf(bc);
    }

    return ret;
}

static uintptr_t
guest_to_host(uintptr_t guest_addr, struct boot_config *bc)
{
    return bc->guest_mem_base + guest_addr;
}

static uintptr_t
host_to_guest(uintptr_t host_addr, struct boot_config *bc)
{
    return host_addr - bc->guest_mem_base;
}

static int
multiboot_load_image(struct boot_config *bc)
{
    size_t image_load_size;
    uintptr_t to = guest_to_host(bc->kernel_load_data.load_addr, bc);
    uintptr_t from = bc->header - (bc->kernel_load_data.header_addr - bc->kernel_load_data.load_addr);

    // if there wasn't a load_end_addr provided, then default it to the length of the image file
    if (bc->kernel_load_data.load_end_addr == 0x0)
        bc->kernel_load_data.load_end_addr = bc->kernel_load_data.load_addr + (uint32_t)bc->file_size -
            (uint32_t)(bc->header - (uintptr_t)bc->mapping);
    image_load_size = bc->kernel_load_data.load_end_addr - bc->kernel_load_data.load_addr;

    memcpy((void *)to, (void *)from, image_load_size);

    return 0;
}

static uint64_t
multiboot_set_guest_state(struct boot_config* bc)
{
    struct multiboot_header* header = (struct multiboot_header *)bc->guest_mem_base;
    uintptr_t guest_header_ptr = host_to_guest((uintptr_t)header, bc);
    header->hdr.flags = 0x1234;

    xh_vcpu_reset(0);
    xh_vm_set_register(0, VM_REG_GUEST_RAX, 0x2BADB002);
    xh_vm_set_register(0, VM_REG_GUEST_RBX, guest_header_ptr);
    xh_vm_set_register(0, VM_REG_GUEST_RIP, bc->kernel_load_data.entry_addr);

    xh_vm_set_desc(0, VM_REG_GUEST_CS, 0, 0xffffffff, 0xc09b);
    xh_vm_set_desc(0, VM_REG_GUEST_DS, 0, 0xffffffff, 0xc093);
    xh_vm_set_desc(0, VM_REG_GUEST_ES, 0, 0xffffffff, 0xc093);
    xh_vm_set_desc(0, VM_REG_GUEST_FS, 0, 0xffffffff, 0xc093);
    xh_vm_set_desc(0, VM_REG_GUEST_GS, 0, 0xffffffff, 0xc093);
    xh_vm_set_desc(0, VM_REG_GUEST_SS, 0, 0xffffffff, 0xc093);

    xh_vm_set_register(0, VM_REG_GUEST_CR0, 0x21); /* enable protected mode */

    return bc->kernel_load_data.entry_addr;
}

uint64_t
multiboot(void)
{
    struct boot_config boot_config;
    struct stat st;
    uint64_t entry;
    int fd;

    memset(&boot_config, 0, sizeof(boot_config));
    fd = open(config.kernel_path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "multiboot: failed to open kernel '%s': %s\n",
                config.kernel_path, strerror(errno));
        abort();
    }
    if (fstat(fd, &st) < 0) {
        fprintf(stderr, "multiboot: failed to stat kernel '%s': %s\n",
                config.kernel_path, strerror(errno));
        close(fd);
        abort();
    }
    boot_config.file_size = (size_t)st.st_size;

    boot_config.mapping = mmap(NULL, (size_t)ROUND_UP(st.st_size, 4096),
            PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (boot_config.mapping == (void *)MAP_FAILED) {
        fprintf(stderr, "multiboot: failed to mmap kernel '%s': %s\n",
                config.kernel_path, strerror(errno));
        abort();
    }

    if (multiboot_find_header(&boot_config)) {
        fprintf(stderr, "multiboot: failed to find multiboot header in '%s'\n",
                config.kernel_path);
        abort();
    }

    // get the guest's memory range
    void *gpa = xh_vm_map_gpa(0, xh_vm_get_lowmem_size());
    boot_config.guest_mem_base = (uintptr_t)gpa;
    boot_config.guest_mem_size = xh_vm_get_lowmem_size();

    // actually load the image into the guest's memory
    if (multiboot_load_image(&boot_config)) {
        fprintf(stderr, "multiboot: failed to load kernel image into "
                "guest's memory\n");
        abort();
    }

    entry = multiboot_set_guest_state(&boot_config);
    munmap(boot_config.mapping, ROUND_UP(boot_config.file_size, 4096));
    return entry;
}
