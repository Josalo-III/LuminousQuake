/* Native portability checks, independent of retail assets. GPL-2.0-or-later. */
#include "quakedef.h"
#include "localization.h"
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
    { extern float SCR_ConsoleAdvance(float, float, float, double, int);
      for (int height = 300; height <= 2400; height *= 2) {
          float current = 0;
          for (int tick = 0; tick < 4; ++tick) current = SCR_ConsoleAdvance(current,height/2,300,0.25,height);
          CHECK(current == height/2);
          CHECK(SCR_ConsoleAdvance(current,0,300,0.5,height) == height/4);
          CHECK(SCR_ConsoleAdvance(current,0,300,2,height) == 0);
      }
    }
    { char fixture[] = "// comment\nmap_jump_across = \"Jump across\\nnow\"\nempty = \"\"\nbroken = unquoted\n"
                       "qc_item_health = \"You receive {0} health\\n\"\n"
                       "qc_got_item = \"You got {0}\\n\"\nqc_armor = \"armor\"\n"
                       "pair = \"{1}/{0}/{1}\"\n";
      LQ_LocalizationParse(fixture);
      CHECK(!strcmp(LQ_Localize("$map_jump_across"), "Jump across\nnow"));
      CHECK(!strcmp(LQ_Localize("$empty"), ""));
      CHECK(!strcmp(LQ_Localize("$missing"), "$missing"));
      CHECK(!strcmp(LQ_Localize("Original text"), "Original text"));
      const char *health[] = {"$qc_item_health","15"}; char output[256];
      CHECK(LQ_LocalizeArgs(2,health,output,sizeof(output)) && !strcmp(output,"You receive 15 health\n"));
      const char *item[] = {"$qc_got_item","$qc_armor"};
      CHECK(LQ_LocalizeArgs(2,item,output,sizeof(output)) && !strcmp(output,"You got armor\n"));
      const char *pair[] = {"$pair","a","b"};
      CHECK(LQ_LocalizeArgs(3,pair,output,sizeof(output)) && !strcmp(output,"b/a/b"));
      const char *literal[] = {"original {0}","100%"};
      CHECK(LQ_LocalizeArgs(2,literal,output,sizeof(output)) && !strcmp(output,"original {0}100%"));
      CHECK(!LQ_LocalizeArgs(2,health,output,4));
      LQ_LocalizationParse(""); }
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
    {
        /* GPU sprites use Mod_Extradata. A sprite lives in the hunk, unlike
         * MDL data: interpreting its preceding bytes as an LRU header crashes. */
        model_t sprite_model = {0}, alias_model = {0};
        mark = Hunk_LowMark();
        msprite_t *sprite = Hunk_AllocName(sizeof(*sprite), "s_explod");
        sprite->numframes = 1; sprite->type = SPR_VP_PARALLEL;
        sprite_model.type = mod_sprite; sprite_model.cache.data = sprite;
        alias_model.type = mod_alias;
        Cache_Alloc(&alias_model.cache, 64, "alias-test");
        memset(alias_model.cache.data, 0x5a, 64);
        CHECK(Mod_Extradata(&sprite_model) == sprite);
        CHECK(Mod_Extradata(&sprite_model) == sprite);
        CHECK(sprite->numframes == 1 && sprite->type == SPR_VP_PARALLEL);
        CHECK(Mod_Extradata(&alias_model) == alias_model.cache.data);
        CHECK(((byte *)alias_model.cache.data)[63] == 0x5a);
        Cache_Free(&alias_model.cache);
        Hunk_FreeToLowMark(mark);
        puts("PASS hunk sprite access and alias cache integrity");
    }
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
        if (COM_CheckParm("--test-localization")) {
            extern char scr_centerstring[];
            CHECK(!strcmp(LQ_Localize("$map_jump_across"), "Fixture jump across\nnow"));
            byte packet[64];
            net_message.data = packet; net_message.maxsize = sizeof(packet); net_message.cursize = 0;
            MSG_WriteByte(&net_message, svc_centerprint);
            MSG_WriteString(&net_message, "$map_jump_across");
            CL_ParseServerMessage();
            CHECK(!strcmp(scr_centerstring, "Fixture jump across\nnow"));
            net_message.data = NULL; net_message.cursize = net_message.maxsize = 0;
            puts("PASS installation localization lookup");
        }
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
        { extern void SV_Physics(void); extern void SV_AddGravity(edict_t *);
          extern cvar_t sv_gravity;
          /* Exercise the actual server dispatcher for type 11. A grounded gib
           * stops before collision tracing, so this fixture needs no BSP. */
          pr_global_struct->StartFrame = 1; pr_global_struct->force_retouch = 0;
          svs.maxclients = 0;
          for (int i = 0; i < 3; ++i) {
              edict_t *ent = EDICT_NUM(i); memset(&ent->v,0,sizeof(ent->v)); ent->free = i == 2;
          }
          edict_t *gib = EDICT_NUM(1); gib->v.movetype = MOVETYPE_GIB; gib->v.flags = FL_ONGROUND;
          SV_Physics(); CHECK(gib->v.movetype == MOVETYPE_GIB);
          float old_gravity = sv_gravity.value, old_frame = host_frametime;
          sv_gravity.value = 800; host_frametime = 0.1f;
          SV_AddGravity(gib); CHECK(gib->v.velocity[2] == -80);
          sv_gravity.value = old_gravity; host_frametime = old_frame;
          puts("PASS rerelease gib movement dispatcher and gravity");
        }
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
