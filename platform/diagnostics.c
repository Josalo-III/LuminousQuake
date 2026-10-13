/* Native portability checks, independent of retail assets. GPL-2.0-or-later. */
#include "quakedef.h"
#include "diagnostics.h"
#include "net_udp.h"
#include <stdalign.h>
void COM_InitFilesystem(void);
int LongSwap(int value);
extern cvar_t hostname;

_Static_assert(sizeof(float) == 4, "QuakeC requires 32-bit floats");
_Static_assert(sizeof(int) == 4, "Quake formats require 32-bit ints");
_Static_assert(sizeof(dstatement_t) == 8, "QuakeC v6 statement layout changed");
_Static_assert(sizeof(ddef_t) == 8, "QuakeC v6 definition layout changed");
_Static_assert(sizeof(dprograms_t) == 60, "QuakeC v6 header layout changed");
_Static_assert(sizeof(dheader_t) == 124, "Quake BSP header layout changed");

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); return 1; \
} } while (0)

int LQ_RunDiagnostics(int argc, char **argv)
{
    void *memory = malloc(32 * 1024 * 1024);
    void *blocks[129];
    int i, mark;
    double start;
    struct linked_test { int prefix; link_t link; } linked;
    struct qsockaddr addr, other;
    COM_InitArgv(argc, argv);
    printf("entityfields=%zu\n", sizeof(entvars_t) / 4 + 1);
    CHECK(memory != NULL);
    CHECK(Q_ClampedInt(3.5) == 3 && Q_ClampedInt(-3.5) == -3);
    CHECK(Q_ClampedInt(3e9) == INT_MAX && Q_ClampedInt(-3e9) == INT_MIN);
    COM_InitByteOrder();
    Memory_Init(memory, 32 * 1024 * 1024);
    CHECK(STRUCT_FROM_LINK(&linked.link, struct linked_test, link) == &linked);
    for (i = 0; i < 129; ++i) {
        blocks[i] = Z_Malloc(i + 1);
        CHECK((uintptr_t)blocks[i] % alignof(void *) == 0);
        memset(blocks[i], i, i + 1);
    }
    for (i = 0; i < 129; i += 2) Z_Free(blocks[i]);
    for (i = 1; i < 129; i += 2) Z_Free(blocks[i]);
    Z_CheckHeap();
    mark = Hunk_LowMark();
    for (i = 1; i < 33; ++i) {
        void *block = Hunk_Alloc(i);
        CHECK((uintptr_t)block % 16 == 0);
    }
    Hunk_FreeToLowMark(mark);
    CHECK(Hunk_LowMark() == mark);
    CHECK(UDP_StringToAddr("127.0.0.1:26000", &addr) == 0);
    CHECK(UDP_GetSocketPort(&addr) == 26000);
    CHECK(!strcmp(UDP_AddrToString(&addr), "127.0.0.1:26000"));
    CHECK(UDP_StringToAddr("300.0.0.1:26000", &other) == -1);
    CHECK(UDP_StringToAddr("127.0.0.1:65536", &other) == -1);
    other = addr;
    CHECK(UDP_AddrCompare(&addr, &other) == 0);
    UDP_SetSocketPort(&other, 26001);
    CHECK(UDP_AddrCompare(&addr, &other) == 1);
    { uint32_t words[4]; byte packet[16];
      Q_memset(words, 255, sizeof(words));
      for (i = 0; i < 4; ++i) CHECK(words[i] == UINT32_MAX);
      memset(&net_message, 0, sizeof(net_message));
      net_message.data = packet; net_message.maxsize = sizeof(packet);
      MSG_WriteLong(&net_message, (int32_t)0x80010203u);
      MSG_WriteFloat(&net_message, 1.25f);
      MSG_BeginReading();
      CHECK((uint32_t)MSG_ReadLong() == 0x80010203u);
      CHECK(MSG_ReadFloat() == 1.25f);
      CHECK(!msg_badread);
      CHECK(MSG_ReadFloat() == -1 && msg_badread); }
    CHECK((uint32_t)LongSwap((int32_t)0x80010203u) == 0x03020180u);
    CHECK((uint32_t)LongSwap((int32_t)0x03020180u) == 0x80010203u);
    /* Rerelease metadata must not misalign the following original message. */
    { byte packet[128]; int saved_state = cls.state;
      cls.state = ca_disconnected;
      net_message.data = packet; net_message.maxsize = sizeof(packet); net_message.cursize = 0;
      MSG_WriteByte(&net_message, svc_achievement);
      MSG_WriteString(&net_message, "ACH_FIND_SECRET");
      MSG_WriteByte(&net_message, svc_updatestat);
      MSG_WriteByte(&net_message, STAT_HEALTH); MSG_WriteLong(&net_message, 101);
      CL_ParseServerMessage();
      CHECK(cl.stats[STAT_HEALTH] == 101 && msg_readcount == net_message.cursize);
      net_message.cursize = 2; packet[0] = svc_achievement; packet[1] = 'X';
      if (!setjmp(host_abortserver)) {
          CL_ParseServerMessage(); CHECK(false);
      }
      CHECK(msg_badread); cls.state = saved_state;
      net_message.data = NULL; net_message.cursize = net_message.maxsize = 0;
    }
    start = Sys_FloatTime(); Sys_Sleep();
    CHECK(Sys_FloatTime() > start);
    { char base[32];
      COM_FileBase("progs.dat", base); CHECK(!strcmp(base, "progs"));
      COM_FileBase("maps/test.bsp", base); CHECK(!strcmp(base, "test"));
      COM_FileBase("", base); CHECK(!strcmp(base, "?model?")); }
    if (COM_CheckParm("--test-udp")) {
        byte sent[] = {0x80, 0x01, 0xfe, 0x7f}, received[4];
        int sender, receiver, bytes = 0;
        Cvar_RegisterVariable(&hostname);
        CHECK(UDP_Init() >= 0);
        sender = UDP_OpenSocket(0); receiver = UDP_OpenSocket(0);
        CHECK(sender >= 0 && receiver >= 0);
        CHECK(UDP_GetSocketAddr(receiver, &addr) == 0);
        CHECK(UDP_GetSocketPort(&addr) != 0);
        CHECK(UDP_Write(sender, sent, sizeof(sent), &addr) == sizeof(sent));
        start = Sys_FloatTime();
        while (!bytes && Sys_FloatTime() - start < 1.0) {
            bytes = UDP_Read(receiver, received, sizeof(received), &other);
            Sys_Sleep();
        }
        CHECK(bytes == sizeof(sent) && !memcmp(sent, received, sizeof(sent)));
        CHECK(UDP_GetAddrFromName(".1:26000", &addr) == 0);
        CHECK(!strcmp(UDP_AddrToString(&addr), "127.0.0.1:26000"));
        UDP_Shutdown();
        puts("PASS native UDP loopback and Quake subnet shorthand");
    }
    if (COM_CheckParm("--test-progs")) {
        extern int pr_depth;
        host_parms.basedir = ".";
        Cbuf_Init(); Cmd_Init(); COM_Init(".");
        PR_LoadProgs();
        CHECK(pr_edict_size % alignof(edict_t) == 0);
        sv.max_edicts = sv.num_edicts = 3;
        sv.edicts = Hunk_Alloc(pr_edict_size * sv.max_edicts);
        for (i = 0; i < sv.num_edicts; ++i) {
            CHECK((uintptr_t)EDICT_NUM(i) % alignof(edict_t) == 0);
            CHECK(NUM_FOR_EDICT(EDICT_NUM(i)) == i);
        }
        pr_globals[100] = 3.5f; pr_globals[101] = 2.25f;
        PR_ExecuteProgram(1);
        CHECK(pr_globals[OFS_RETURN] == 5.75f);
        G_INT(103) = (int)EDICT_TO_PROG(EDICT_NUM(1));
        G_INT(104) = offsetof(entvars_t, health) / 4;
        PR_ExecuteProgram(2);
        CHECK(EDICT_NUM(1)->v.health == 5.75f);
        CHECK(pr_globals[OFS_RETURN] == 5.75f);
        CHECK(pr_depth == 0);
        puts("PASS QuakeC v6 loader, odd-field entity alignment, arithmetic, entity store/load");
    }
    free(memory);
    puts("PASS native layouts, pointer alignment, allocator reuse, IPv4 addresses, monotonic clock");
    return 0;
}

int LQ_CheckData(quakeparms_t *parms)
{
    static const char *required[] = {
        "gfx.wad", "gfx/palette.lmp", "gfx/colormap.lmp", "progs.dat"
    };
    int missing = 0;
    size_t i;
    host_parms = *parms;
    Memory_Init(parms->membase, parms->memsize);
    Cbuf_Init(); Cmd_Init();
    /* Filesystem discovery works before registration/game initialization. */
    COM_InitByteOrder();
    COM_InitFilesystem();
    for (i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        int handle, size;
        size = COM_OpenFile((char *)required[i], &handle);
        printf("%s: %s\n", required[i], handle == -1 ? "missing" : "found");
        if (handle != -1) COM_CloseFile(handle); else ++missing;
        (void)size;
    }
    if (missing) fprintf(stderr, "Original Quake data is missing. Supply -basedir PATH containing id1/.\n");
    else puts("Required files found; this is a presence check, not gameplay validation.");
    return missing ? 2 : 0;
}
