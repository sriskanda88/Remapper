/*
 * main.c
 *
 *  Created on: Oct 21, 2013
 *      Author: skanda
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <elf.h>

/* type definitions for easing transition */
typedef Elf32_Addr Address;
typedef Elf32_Off Offset;
typedef uint32_t Size;

//Structs
typedef struct vpaddr {
	Elf32_Addr vaddr;
	Elf32_Addr paddr;
} t_vpaddr;

struct v2pmap {
	t_vpaddr *map;
	int size;
	int last;
};

//Arch of the executable
enum Arch {
	x86, ARM
} ARCH = ARM;

#define V2P_MEM 10000
#define PAGE_SIZE 4096
#define _FILE_OFFSET_BITS 64

uint32_t intreg[32];
t_vpaddr SPC;
t_vpaddr ESP;
int spc_size = 0;

struct v2pmap v2p;

char *cpt_file_name;
char *mem_file_name;
char *remapped_file_name;

Address find_paddr(Address vaddr);
int prepare_addr_lookup_table();
int extract_int(FILE *fp, char delim, int type);
void read_registers_from_checkpoint(FILE *cpt_file);
void build_v2p_map(FILE *cpt_file);
int find(FILE *fp, const char *str, int len);

void create_memmap_file();
void init_v2p_map(struct v2pmap *v2p_map, int size);
void insert_v2p_entry(struct v2pmap *v2p_map, Elf32_Addr vaddr,
		Elf32_Addr paddr);

int main(int argc, char **argv) {

	if (argc < 3) {
		printf ("Usage : Remapper <m5.cpt> <system.physmem> <remapped_file>\n");
		exit(-1);
	} else {
		cpt_file_name = argv[1];
		mem_file_name = argv[2];
		remapped_file_name = argv[3];
	}

//	cpt_file_name = "/home/skanda/proj/gem5/m5out/targetdir/m5.cpt";
//	mem_file_name = "/home/skanda/proj/gem5/m5out/targetdir/system.physmem.store0.pmem";
//	remapped_file_name = "/home/skanda/proj/gem5/m5out/targetdir/remapped_file";

	init_v2p_map(&v2p, V2P_MEM);
	prepare_addr_lookup_table();
	create_memmap_file();

	return 0;
}

Address find_paddr(Address vaddr) {
	int n;

	for (n = 0; n < v2p.last; n++) {
		if (v2p.map[n].vaddr <= vaddr && vaddr <= (v2p.map[n].vaddr + 4096))
			break;
	}

	if (n == v2p.last) {
		//not found
		return -1;
	}

	Offset offset = vaddr - v2p.map[n].vaddr;
	return v2p.map[n].paddr + offset;
}

int prepare_addr_lookup_table() {
	FILE *cpt_file = NULL;

	cpt_file = fopen(cpt_file_name, "r");

	/* Read registers from checkpoint. */
	read_registers_from_checkpoint(cpt_file);

	/* Populate data cache from CPT file. */
	build_v2p_map(cpt_file);

	fclose(cpt_file);

	//end_logging();
	return 0;
}

int extract_int(FILE *fp, char delim, int type) {
	int num = 0;
	char ch = '\0';

	if (type == 0) {
		while (!feof(fp)) {
			ch = fgetc(fp);
			if (delim == ch)
				break;
			num = num * 10 + ch - 48;
		}
		if (ch == delim)
			return num;
		return -1;
	} else {
		ch = fgetc(fp);
		ch = fgetc(fp);
		while (!feof(fp)) {
			ch = fgetc(fp);
			if (delim == ch)
				break;
			if (ch >= 'a' && ch <= 'f')
				ch = ch - 'a' + 58;
			num = num * 16 + ch - 48;
		}
		if (ch == delim)
			return num;
		return -1;
	}
}

void read_registers_from_checkpoint(FILE *cpt_file) {
	int i = 0;

	/* Get integer registers. */
	find(cpt_file, "intRegs=", strlen("intRegs="));
	for (i = 0; i < 32; i++) {
		intreg[i] = extract_int(cpt_file, ' ', 0);
	}

	find(cpt_file, "\n_pc=", strlen("\n_pc="));
	spc_size = ftell(cpt_file);
	SPC.vaddr = extract_int(cpt_file, '\n', 0);

	spc_size = ftell(cpt_file) - spc_size - 2;

	ESP.vaddr = intreg[4];
}

void build_v2p_map(FILE *cpt_file) {
	Elf32_Addr vaddr = 0, paddr = 0;
	char *vpattern, *ppattern;

	fseek(cpt_file, 0, SEEK_SET);

	vpattern = "vaddr=";

	if (ARCH == x86) {
		ppattern = "paddr="; //for x86
	} else if (ARCH == ARM) {
		ppattern = "pfn="; //for ARM
	}

	while (find(cpt_file, vpattern, strlen(vpattern))) {

		vaddr = extract_int(cpt_file, '\n', 0);
		find(cpt_file, ppattern, strlen(ppattern));
		paddr = extract_int(cpt_file, '\n', 0);

		if (ARCH == ARM)
			paddr = paddr * PAGE_SIZE; //only for ARM

		insert_v2p_entry(&v2p, vaddr, paddr);

		if (ARCH == x86)
			find(cpt_file, vpattern, strlen(vpattern)); //only for x86
	}
}

int find(FILE *fp, const char *str, int len) {
	int i;
	char ch = "0";

	while (!feof(fp)) {
		for (i = 0; i < len; i++) {
			//printf("%c == ", ch);
			ch = fgetc(fp);
			//printf("%c\n", ch);
			if (str[i] == ch)
				continue;
			break;
		}
		if (i == len)
			return 1;
	}

	return 0;
}

void create_memmap_file() {

	Address min_vaddr = 0xffffffff;
	Address max_vaddr = 0;
	Address vaddr, paddr;
	int i;
	char buf[4096];

	FILE *fr, *fw;
	fr = fopen(mem_file_name, "r");

	if (access(remapped_file_name, F_OK) != -1) {
		unlink(remapped_file_name);
	}

	fw = fopen(remapped_file_name, "ab+");

	for (i = 0; i < v2p.last; i++) {
		if (v2p.map[i].vaddr < min_vaddr && v2p.map[i].vaddr != 0) {
			min_vaddr = v2p.map[i].vaddr;
		}
		if (v2p.map[i].vaddr > max_vaddr) {
			max_vaddr = v2p.map[i].vaddr;
		}
	}

	for (vaddr = min_vaddr; vaddr <= max_vaddr; vaddr += PAGE_SIZE) {

		if ((paddr = find_paddr(vaddr)) == -1)
			continue;

		fseek(fr, paddr, SEEK_SET);
		fread(&buf, sizeof(char), PAGE_SIZE, fr);
		fwrite(&buf, sizeof(char), PAGE_SIZE, fw);
	}

	fclose(fr);
	fclose(fw);

}

void init_v2p_map(struct v2pmap *v2p_map, int size) {
	v2p_map->map = (t_vpaddr *) calloc(size, sizeof(t_vpaddr));
	v2p_map->size = size;
	v2p_map->last = 0;
}

void insert_v2p_entry(struct v2pmap *v2p_map, Elf32_Addr vaddr,
		Elf32_Addr paddr) {
	if (v2p_map->size - 1 == v2p_map->last) {
		v2p_map->map = (t_vpaddr *) realloc(v2p_map->map,
				2 * v2p_map->size * sizeof(t_vpaddr));
		v2p_map->size = v2p_map->size * 2;
	}
	v2p_map->last++;
	v2p_map->map[v2p_map->last].vaddr = vaddr;
	v2p_map->map[v2p_map->last].paddr = paddr;
}
