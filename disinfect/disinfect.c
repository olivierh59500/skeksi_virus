#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <elf.h>
#include <errno.h>
#include <pthread.h>
#include <sys/ptrace.h>
#include <sys/user.h>

typedef struct elfdesc {
	Elf64_Ehdr *ehdr;
	Elf64_Phdr *phdr;
	Elf64_Shdr *shdr;
	Elf64_Addr textVaddr;
	Elf64_Addr dataVaddr;
	size_t textSize;
	size_t dataSize;
	uint8_t *mem;
	struct stat st;
	char *path;
} elfdesc_t;

#define TMP ".disinfect_file.xyz"

uint32_t locate_glibc_init_offset(elfdesc_t *elf)
{
	uint32_t i;
	uint8_t *mem = elf->mem;
	for (i = 0; i < elf->st.st_size; i++) {
		if (
		mem[0] == 0x41 && mem[1] == 0x57 && 
		mem[2] == 0x41 && mem[3] == 0x56 && 
		mem[4] == 0x41 && mem[5] == 0x55 &&
		mem[6] == 0x41 && mem[7] == 0x54)  // probable glibc initialization code
			return i;
	}
	return 0;
}
	
/*
 * Expected x86_64 base is 0x400000 in Linux. We rely on that
 * here, which may end up being a bit wobbly.
 */
int disinfect(elfdesc_t *elf)
{
	size_t paddingSize;
	Elf64_Phdr *phdr = elf->phdr;
	Elf64_Shdr *shdr = elf->shdr;
	uint32_t text_offset = 0;
	char *strtab = NULL;
	uint8_t *mem = elf->mem;
	int i, textfound, fd;
	ssize_t c, last_chunk;
	if (elf->textVaddr >= 0x400000) {
		printf("unexpected text segment address, this file may not actually be infected\n");
		return -1;
	}
	paddingSize = 0x400000 - elf->textVaddr;
	
	/*
	 * PT_PHDR, PT_INTERP were pushed forward in the file
	 */
	phdr[0].p_offset -= paddingSize;
	phdr[1].p_offset -= paddingSize;
	
	/*
	 * Set phdr's back to normal
	 */
	for (textfound = 0, i = 0; i < elf->ehdr->e_phnum; i++) {
		if (textfound) {
			phdr[i].p_offset -= paddingSize;
			continue;
		}
		if (phdr[i].p_type == PT_LOAD && phdr[i].p_offset == 0 && phdr[i].p_flags & PF_X) {
			if (phdr[i].p_paddr == phdr[i].p_vaddr) {
				phdr[i].p_vaddr += paddingSize;
				phdr[i].p_paddr += paddingSize;
			} else
				phdr[i].p_vaddr += paddingSize;
			textfound = 1;
		}
	}
	/*
	 * Straighten out section headers
	 */
	printf("offset of strtab: %x\n", shdr[elf->ehdr->e_shstrndx].sh_offset);
	strtab = (char *)&mem[shdr[elf->ehdr->e_shstrndx].sh_offset];
	printf("strtab: %p\n", strtab);
	for (i = 0; i < elf->ehdr->e_shnum; i++) {
		printf("shdr[%d].sh_name: %d\n", shdr[i].sh_name);
		/*
	 	 * We treat .text section special because it is modified to 
		 * encase the entire parasite code. Lets change it back to 
		 * only encasing the regular .text stuff.
		 */
		if (!strcmp(&strtab[shdr[i].sh_name], ".text")) {
			text_offset = locate_glibc_init_offset(elf);
			if (text_offset == 0) // leave unchanged :(
				continue;
			shdr[i].sh_offset = text_offset;
			shdr[i].sh_addr = text_offset + 0x400000;
			shdr[i].sh_size -= paddingSize;
			continue;
		}
		shdr[i].sh_offset -= paddingSize;
	}
	
	elf->ehdr->e_shoff -= paddingSize;
	elf->ehdr->e_phoff -= paddingSize;
                 
	if ((fd = open(TMP, O_CREAT | O_TRUNC | O_WRONLY, elf->st.st_mode)) < 0) 
		return -1;

	if ((c = write(fd, mem, sizeof(Elf64_Ehdr))) != sizeof(Elf64_Ehdr)) 
		return -1;

	mem += paddingSize + sizeof(Elf64_Ehdr);
	last_chunk = elf->st.st_size - (paddingSize + sizeof(Elf64_Ehdr));
	
	if ((c = write(fd, mem, last_chunk)) != last_chunk) 
		return -1;

	if (fchown(fd, elf->st.st_uid, elf->st.st_gid) < 0)
		return -1;

	rename(elf->path, TMP);
	return 0;
}

int load_executable(const char *path, elfdesc_t *elf)
{
	uint8_t *mem;
	Elf64_Ehdr *ehdr;
	Elf64_Phdr *phdr;
	Elf64_Shdr *shdr;
	int fd;
	struct stat st;
	int i;

	if ((fd = open(path, O_RDONLY)) < 0) {
		perror("open");
		return -1;
	}
	fstat(fd, &st);
	
	mem = mmap(NULL, st.st_size, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (mem == MAP_FAILED) {
		perror("mmap");
		return -1;
	}
	
	ehdr = (Elf64_Ehdr *)mem;
	phdr = (Elf64_Phdr *)&mem[ehdr->e_phoff];
	shdr = (Elf64_Shdr *)&mem[ehdr->e_shoff];
	
	elf->st = st;
	
	for (i = 0; i < ehdr->e_phnum; i++) {
		switch(!!phdr[i].p_offset) {
			case 0:
				elf->textVaddr = phdr[i].p_vaddr;
				elf->textSize = phdr[i].p_filesz;
				break;
			case 1:
				elf->dataVaddr = phdr[i].p_vaddr;
				elf->dataSize = phdr[i].p_filesz;
				break;
		}
	}
	elf->mem = mem;
	elf->ehdr = ehdr;
	elf->phdr = phdr;
	elf->shdr = shdr;
	elf->path = (char *)path;
	return 0;
	
}
	
int test_for_skeksi(elfdesc_t *elf)
{
	uint32_t magic = *(uint32_t *)&elf->ehdr->e_ident[EI_PAD];
	return (magic == 0x15D25); 
}

int main(int argc, char **argv)
{
	elfdesc_t elf;

	if (argc < 2) {
		printf("Usage: %s <executable>\n", argv[0]);
		exit(0);
	}
	
	if (load_executable(argv[1], &elf) < 0) {
		printf("Failed to load executable: %s\n", argv[1]);
		exit(-1);
	}
	
	if (test_for_skeksi(&elf) == 0) {
		printf("File: %s, is not infected with the Skeksi virus\n", argv[1]);
		exit(-1);
	}
	
	if (disinfect(&elf) < 0) {
		printf("Failed to disinfect file: %s\n", argv[1]);
		exit(-1);
	}

	printf("Successfully disinfected: %s\n", argv[1]);
	
	
}

