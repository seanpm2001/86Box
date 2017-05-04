#include <stdio.h>
#include "ibm.h"
#include "io.h"
#include "nvr.h"
#include "pic.h"
#include "timer.h"
#include "rtc.h"

int oldromset;
int nvrmask=63;
char nvrram[128];
int nvraddr;

int nvr_dosave = 0;

static int nvr_onesec_time = 0, nvr_onesec_cnt = 0;

static int rtctime;

void getnvrtime()
{
	time_get(nvrram);
}

void nvr_recalc()
{
        int c;
        int newrtctime;
        c = 1 << ((nvrram[RTC_REGA] & RTC_RS) - 1);
        newrtctime=(int)(RTCCONST * c * (1 << TIMER_SHIFT));
        if (rtctime>newrtctime) rtctime=newrtctime;
}

void nvr_rtc(void *p)
{
        int c;
        if (!(nvrram[RTC_REGA] & RTC_RS))
        {
                rtctime=0x7fffffff;
                return;
        }
        c = 1 << ((nvrram[RTC_REGA] & RTC_RS) - 1);
        rtctime += (int)(RTCCONST * c * (1 << TIMER_SHIFT));
        nvrram[RTC_REGC] |= RTC_PF;
        if (nvrram[RTC_REGB] & RTC_PIE)
        {
                nvrram[RTC_REGC] |= RTC_IRQF;
                if (AMSTRAD) picint(2);
                else         picint(0x100);
        }
}

int nvr_update_status = 0;

#define ALARM_DONTCARE	0xc0

int nvr_check_alarm(int nvraddr)
{
        return (nvrram[nvraddr + 1] == nvrram[nvraddr] || (nvrram[nvraddr + 1] & ALARM_DONTCARE) == ALARM_DONTCARE);
}

int nvr_update_end_count = 0;

void nvr_update_end(void *p)
{
        if (!(nvrram[RTC_REGB] & RTC_SET))
        {
                getnvrtime();
                /* Clear update status. */
                nvr_update_status = 0;

                if (nvr_check_alarm(RTC_SECONDS) && nvr_check_alarm(RTC_MINUTES) && nvr_check_alarm(RTC_HOURS))
                {
                        nvrram[RTC_REGC] |= RTC_AF;
                        if (nvrram[RTC_REGB] & RTC_AIE)
                        {
                                nvrram[RTC_REGC] |= RTC_IRQF;
                                if (AMSTRAD) picint(2);
                                else         picint(0x100);
                        }
                }

                /* The flag and interrupt should be issued on update ended, not started. */
                nvrram[RTC_REGC] |= RTC_UF;
                if (nvrram[RTC_REGB] & RTC_UIE)
                {
                        nvrram[RTC_REGC] |= RTC_IRQF;
                        if (AMSTRAD) picint(2);
                        else         picint(0x100);
                }
        }

        nvr_update_end_count = 0;
}

void nvr_onesec(void *p)
{
        nvr_onesec_cnt++;
        if (nvr_onesec_cnt >= 100)
        {
                if (!(nvrram[RTC_REGB] & RTC_SET))
                {
                        nvr_update_status = RTC_UIP;
                        rtc_tick();

                        nvr_update_end_count = (int)((244.0 + 1984.0) * TIMER_USEC);
                }
                nvr_onesec_cnt = 0;
        }
        nvr_onesec_time += (int)(10000 * TIMER_USEC);
}

void writenvr(uint16_t addr, uint8_t val, void *priv)
{
        int c, old;
        if (addr&1)
        {
                if (nvraddr==RTC_REGC || nvraddr==RTC_REGD)
                        return; /* Registers C and D are read-only. There's no reason to continue. */
                if (nvraddr > RTC_REGD && nvrram[nvraddr] != val)
                   nvr_dosave = 1;
                
		old = nvrram[nvraddr];
                nvrram[nvraddr]=val;

                if (nvraddr == RTC_REGA)
                {
                        if (val & RTC_RS)
                        {
                                c = 1 << ((val & RTC_RS) - 1);
                                rtctime += (int)(RTCCONST * c * (1 << TIMER_SHIFT));
                        }
                        else
                           rtctime = 0x7fffffff;
                }
		else
		{
                        if (nvraddr == RTC_REGB)
                        {
                                if (((old ^ val) & RTC_SET) && (val & RTC_SET))
                                {
                                        nvrram[RTC_REGA] &= ~RTC_UIP;             /* This has to be done according to the datasheet. */
                                        nvrram[RTC_REGB] &= ~RTC_UIE;             /* This also has to happen per the specification. */
                                }
                        }

                        if ((nvraddr < RTC_REGA) || (nvraddr == RTC_CENTURY))
                        {
                                if ((nvraddr != 1) && (nvraddr != 3) && (nvraddr != 5))
                                {
                                        if ((old != val) && !enable_sync)
                                        {
                                                time_update(nvrram, nvraddr);
                                                nvr_dosave = 1;
                                        }
                                }
                        }
                }
        }
        else        nvraddr=val&nvrmask;
}

uint8_t readnvr(uint16_t addr, void *priv)
{
        uint8_t temp;
        if (addr&1)
        {
                if (nvraddr == RTC_REGA)
                        return ((nvrram[RTC_REGA] & 0x7F) | nvr_update_status);
                if (nvraddr == RTC_REGD)
                        nvrram[RTC_REGD] |= RTC_VRT;
                if (nvraddr == RTC_REGC)
                {
                        if (AMSTRAD) picintc(2);
                        else         picintc(0x100);
                        temp = nvrram[RTC_REGC];
                        nvrram[RTC_REGC] = 0;
                        return temp;
                }
                return nvrram[nvraddr];
        }
        return nvraddr;
}

void loadnvr()
{
        FILE *f;
        int c;
        nvrmask=63;
        oldromset=romset;
        switch (romset)
        {
                case ROM_PC1512:      f = romfopen(nvr_concat("pc1512.nvr"),      "rb"); break;
                case ROM_PC1640:      f = romfopen(nvr_concat("pc1640.nvr"),      "rb"); break;
                case ROM_PC200:       f = romfopen(nvr_concat("pc200.nvr"),       "rb"); break;
                case ROM_PC2086:      f = romfopen(nvr_concat("pc2086.nvr"),      "rb"); break;
                case ROM_PC3086:      f = romfopen(nvr_concat("pc3086.nvr"),      "rb"); break;                
                case ROM_IBMAT:       f = romfopen(nvr_concat("at.nvr"),          "rb"); break;
                case ROM_IBMPS1_2011: f = romfopen(nvr_concat("ibmps1_2011.nvr"), "rb"); nvrmask = 127; break;
                case ROM_IBMPS1_2121: f = romfopen(nvr_concat("ibmps1_2121.nvr"), "rb"); nvrmask = 127; break;
                case ROM_IBMPS1_2121_ISA: f = romfopen(nvr_concat("ibmps1_2121_isa.nvr"), "rb"); nvrmask = 127; break;
                case ROM_IBMPS2_M30_286: f = romfopen(nvr_concat("ibmps2_m30_286.nvr"), "rb"); nvrmask = 127; break;
                case ROM_IBMPS2_M50:  f = romfopen("nvr/ibmps2_m50.nvr",  "rb"); break;
                case ROM_IBMPS2_M55SX: f = romfopen("nvr/ibmps2_m55sx.nvr",  "rb"); break;
                case ROM_IBMPS2_M80:  f = romfopen("nvr/ibmps2_m80.nvr",  "rb"); break;
                case ROM_CMDPC30:     f = romfopen(nvr_concat("cmdpc30.nvr"),     "rb"); nvrmask = 127; break;
				case ROM_PORTABLEII:  f = romfopen(nvr_concat("portableii.nvr"),  "rb"); break;
				case ROM_PORTABLEIII: f = romfopen(nvr_concat("portableiii.nvr"),  "rb"); break;
                case ROM_AMI286:      f = romfopen(nvr_concat("ami286.nvr"),      "rb"); nvrmask = 127; break;
                case ROM_AWARD286:    f = romfopen(nvr_concat("award286.nvr"),    "rb"); nvrmask = 127; break;
                case ROM_DELL200:     f = romfopen(nvr_concat("dell200.nvr"),     "rb"); nvrmask = 127; break;
                case ROM_SUPER286TR:  f = romfopen(nvr_concat("super286tr.nvr"),  "rb"); nvrmask = 127; break;
                case ROM_SPC4200P:    f = romfopen(nvr_concat("spc4200p.nvr"),    "rb"); nvrmask = 127; break;
                case ROM_IBMAT386:    f = romfopen(nvr_concat("at386.nvr"),       "rb"); nvrmask = 127; break;
                case ROM_DESKPRO_386: f = romfopen(nvr_concat("deskpro386.nvr"),  "rb"); break;
				case ROM_PORTABLEIII386: f = romfopen(nvr_concat("portableiii386.nvr"),  "rb"); break;
                /* case ROM_ACER386:     f = romfopen(nvr_concat("acer386.nvr"),     "rb"); nvrmask = 127; break; */
                case ROM_MEGAPC:      f = romfopen(nvr_concat("megapc.nvr"),      "rb"); nvrmask = 127; break;
                case ROM_MEGAPCDX:    f = romfopen(nvr_concat("megapcdx.nvr"),    "rb"); nvrmask = 127; break;
                case ROM_AMI386SX:    f = romfopen(nvr_concat("ami386.nvr"),      "rb"); nvrmask = 127; break;
                case ROM_AMI486:      f = romfopen(nvr_concat("ami486.nvr"),      "rb"); nvrmask = 127; break;
                case ROM_WIN486:      f = romfopen(nvr_concat("win486.nvr"),      "rb"); nvrmask = 127; break;
                case ROM_PCI486:      f = romfopen(nvr_concat("hot-433.nvr"),     "rb"); nvrmask = 127; break;
                case ROM_SIS496:      f = romfopen(nvr_concat("sis496.nvr"),      "rb"); nvrmask = 127; break;
                case ROM_430VX:       f = romfopen(nvr_concat("430vx.nvr"),       "rb"); nvrmask = 127; break;
                case ROM_REVENGE:     f = romfopen(nvr_concat("revenge.nvr"),     "rb"); nvrmask = 127; break;
                case ROM_ENDEAVOR:    f = romfopen(nvr_concat("endeavor.nvr"),    "rb"); nvrmask = 127; break;
                /* case ROM_PX386:       f = romfopen(nvr_concat("px386.nvr"),       "rb"); nvrmask = 127; break; */
                case ROM_DTK386:      f = romfopen(nvr_concat("dtk386.nvr"),      "rb"); nvrmask = 127; break;
                case ROM_MR386DX_OPTI495:  f = romfopen(nvr_concat("mr386dx_opti495.nvr"),  "rb"); nvrmask = 127; break;
                case ROM_AMI386DX_OPTI495: f = romfopen(nvr_concat("ami386dx_opti495.nvr"), "rb"); nvrmask = 127; break;
                case ROM_DTK486:      f = romfopen(nvr_concat("dtk486.nvr"),      "rb"); nvrmask = 127; break;
                case ROM_R418:        f = romfopen(nvr_concat("r418.nvr"),        "rb"); nvrmask = 127; break;
                case ROM_586MC1:      f = romfopen(nvr_concat("586mc1.nvr"),      "rb"); nvrmask = 127; break;
                case ROM_PLATO:       f = romfopen(nvr_concat("plato.nvr"),       "rb"); nvrmask = 127; break;
                case ROM_MB500N:      f = romfopen(nvr_concat("mb500n.nvr"),      "rb"); nvrmask = 127; break;
#if 0
                case ROM_POWERMATE_V: f = romfopen(nvr_concat("powermate_v.nvr"), "rb"); nvrmask = 127; break;
#endif
                case ROM_P54TP4XE:    f = romfopen(nvr_concat("p54tp4xe.nvr"),    "rb"); nvrmask = 127; break;
                case ROM_AP53:        f = romfopen(nvr_concat("ap53.nvr"),        "rb"); nvrmask = 127; break;
                case ROM_P55T2S:      f = romfopen(nvr_concat("p55t2s.nvr"),      "rb"); nvrmask = 127; break;
                case ROM_ACERM3A:     f = romfopen(nvr_concat("acerm3a.nvr"),     "rb"); nvrmask = 127; break;
                case ROM_ACERV35N:    f = romfopen(nvr_concat("acerv35n.nvr"),    "rb"); nvrmask = 127; break;
                case ROM_P55VA:       f = romfopen(nvr_concat("p55va.nvr"),       "rb"); nvrmask = 127; break;
                case ROM_P55T2P4:     f = romfopen(nvr_concat("p55t2p4.nvr"),     "rb"); nvrmask = 127; break;
                case ROM_P55TVP4:     f = romfopen(nvr_concat("p55tvp4.nvr"),     "rb"); nvrmask = 127; break;
                case ROM_440FX:       f = romfopen(nvr_concat("440fx.nvr"),       "rb"); nvrmask = 127; break;
#if 0
                case ROM_MARL:        f = romfopen(nvr_concat("marl.nvr"),        "rb"); nvrmask = 127; break;
#endif
                case ROM_THOR:        f = romfopen(nvr_concat("thor.nvr"),        "rb"); nvrmask = 127; break;
                case ROM_MRTHOR:      f = romfopen(nvr_concat("mrthor.nvr"),      "rb"); nvrmask = 127; break;
                case ROM_ZAPPA:       f = romfopen(nvr_concat("zappa.nvr"),       "rb"); nvrmask = 127; break;
#if 0
                case ROM_CMDPC60:     f = romfopen(nvr_concat("cmdpc60.nvr"),     "rb"); nvrmask = 127; break;
#endif
                case ROM_S1668:       f = romfopen(nvr_concat("tpatx.nvr"),       "rb"); nvrmask = 127; break;
                default: return;
        }
        if (!f)
        {
                memset(nvrram,0xFF,128);
                if (!enable_sync)
                {
                        nvrram[RTC_SECONDS] = nvrram[RTC_MINUTES] = nvrram[RTC_HOURS] = 0;
                        nvrram[RTC_DOM] = nvrram[RTC_MONTH] = 1;
                        nvrram[RTC_YEAR] = BCD(80);
                        nvrram[RTC_CENTURY] = BCD(19);
                        nvrram[RTC_REGB] = RTC_2412;
                }
                return;
        }
        fread(nvrram,128,1,f);
        if (enable_sync)
                time_internal_sync(nvrram);
        else
                time_internal_set_nvrram(nvrram); /* Update the internal clock state based on the NVR registers. */
        fclose(f);
        nvrram[RTC_REGA] = 6;
        nvrram[RTC_REGB] = RTC_2412;
        c = 1 << ((nvrram[RTC_REGA] & RTC_RS) - 1);
        rtctime += (int)(RTCCONST * c * (1 << TIMER_SHIFT));
}
void savenvr()
{
        FILE *f;
        switch (oldromset)
        {
                case ROM_PC1512:      f = romfopen(nvr_concat("pc1512.nvr"),      "wb"); break;
                case ROM_PC1640:      f = romfopen(nvr_concat("pc1640.nvr"),      "wb"); break;
                case ROM_PC200:       f = romfopen(nvr_concat("pc200.nvr"),       "wb"); break;
                case ROM_PC2086:      f = romfopen(nvr_concat("pc2086.nvr"),      "wb"); break;
                case ROM_PC3086:      f = romfopen(nvr_concat("pc3086.nvr"),      "wb"); break;
                case ROM_IBMAT:       f = romfopen(nvr_concat("at.nvr"),          "wb"); break;
                case ROM_IBMPS1_2011: f = romfopen(nvr_concat("ibmps1_2011.nvr"), "wb"); break;
                case ROM_IBMPS1_2121: f = romfopen(nvr_concat("ibmps1_2121.nvr"), "wb"); break;
                case ROM_IBMPS1_2121_ISA: f = romfopen(nvr_concat("ibmps1_2121_isa.nvr"), "wb"); break;
                case ROM_IBMPS2_M30_286: f = romfopen(nvr_concat("ibmps2_m30_286.nvr"), "wb"); break;
                case ROM_IBMPS2_M50:  f = romfopen("nvr/ibmps2_m50.nvr",  "wb"); break;
                case ROM_IBMPS2_M55SX: f = romfopen("nvr/ibmps2_m55sx.nvr",  "wb"); break;
                case ROM_IBMPS2_M80:  f = romfopen("nvr/ibmps2_m80.nvr",  "wb"); break;
                case ROM_CMDPC30:     f = romfopen(nvr_concat("cmdpc30.nvr"),     "wb"); break;
				case ROM_PORTABLEII: f = romfopen(nvr_concat("portableii.nvr"),  "wb"); break;
				case ROM_PORTABLEIII: f = romfopen(nvr_concat("portableiii.nvr"),  "wb"); break;
                case ROM_AMI286:      f = romfopen(nvr_concat("ami286.nvr"),      "wb"); break;
                case ROM_AWARD286:    f = romfopen(nvr_concat("award286.nvr"),    "wb"); break;
                case ROM_DELL200:     f = romfopen(nvr_concat("dell200.nvr"),     "wb"); break;
                case ROM_SUPER286TR:  f = romfopen(nvr_concat("super286tr.nvr"),  "wb"); break;
                case ROM_SPC4200P:    f = romfopen(nvr_concat("spc4200p.nvr"),    "wb"); break;
                case ROM_IBMAT386:    f = romfopen(nvr_concat("at386.nvr"),       "wb"); break;
                case ROM_DESKPRO_386: f = romfopen(nvr_concat("deskpro386.nvr"),  "wb"); break;
				case ROM_PORTABLEIII386: f = romfopen(nvr_concat("portableiii386.nvr"),  "wb"); break;
                /* case ROM_ACER386:     f = romfopen(nvr_concat("acer386.nvr"),     "wb"); break; */
                case ROM_MEGAPC:      f = romfopen(nvr_concat("megapc.nvr"),      "wb"); break;
                case ROM_MEGAPCDX:    f = romfopen(nvr_concat("megapcdx.nvr"),    "wb"); break;
                case ROM_AMI386SX:    f = romfopen(nvr_concat("ami386.nvr"),      "wb"); break;
                case ROM_AMI486:      f = romfopen(nvr_concat("ami486.nvr"),      "wb"); break;
                case ROM_WIN486:      f = romfopen(nvr_concat("win486.nvr"),      "wb"); break;
                case ROM_PCI486:      f = romfopen(nvr_concat("hot-433.nvr"),     "wb"); break;
                case ROM_SIS496:      f = romfopen(nvr_concat("sis496.nvr"),      "wb"); break;
                case ROM_430VX:       f = romfopen(nvr_concat("430vx.nvr"),       "wb"); break;
                case ROM_REVENGE:     f = romfopen(nvr_concat("revenge.nvr"),     "wb"); break;
                case ROM_ENDEAVOR:    f = romfopen(nvr_concat("endeavor.nvr"),    "wb"); break;
                /* case ROM_PX386:       f = romfopen(nvr_concat("px386.nvr"),       "wb"); break; */
                case ROM_DTK386:      f = romfopen(nvr_concat("dtk386.nvr"),      "wb"); break;
                case ROM_MR386DX_OPTI495:  f = romfopen(nvr_concat("mr386dx_opti495.nvr"),  "wb"); break;
                case ROM_AMI386DX_OPTI495: f = romfopen(nvr_concat("ami386dx_opti495.nvr"), "wb"); break;
                case ROM_DTK486:      f = romfopen(nvr_concat("dtk486.nvr"),      "wb"); break;
                case ROM_R418:        f = romfopen(nvr_concat("r418.nvr"),        "wb"); break;
                case ROM_586MC1:      f = romfopen(nvr_concat("586mc1.nvr"),      "wb"); break;
                case ROM_PLATO:       f = romfopen(nvr_concat("plato.nvr"),       "wb"); break;
                case ROM_MB500N:      f = romfopen(nvr_concat("mb500n.nvr"),      "wb"); break;
#if 0
                case ROM_POWERMATE_V: f = romfopen(nvr_concat("powermate_v.nvr"), "wb"); break;
#endif
                case ROM_P54TP4XE:    f = romfopen(nvr_concat("p54tp4xe.nvr"),    "wb"); break;
                case ROM_AP53:        f = romfopen(nvr_concat("ap53.nvr"),        "wb"); break;
                case ROM_P55T2S:      f = romfopen(nvr_concat("p55t2s.nvr"),      "wb"); break;
                case ROM_ACERM3A:     f = romfopen(nvr_concat("acerm3a.nvr"),     "wb"); break;
                case ROM_ACERV35N:    f = romfopen(nvr_concat("acerv35n.nvr"),    "wb"); break;
                case ROM_P55VA:       f = romfopen(nvr_concat("p55va.nvr"),       "wb"); break;
                case ROM_P55T2P4:     f = romfopen(nvr_concat("p55t2p4.nvr"),     "wb"); break;
                case ROM_P55TVP4:     f = romfopen(nvr_concat("p55tvp4.nvr"),     "wb"); break;
                case ROM_440FX:       f = romfopen(nvr_concat("440fx.nvr"),       "wb"); break;
#if 0
                case ROM_MARL:        f = romfopen(nvr_concat("marl.nvr"),        "wb"); break;
#endif
                case ROM_THOR:        f = romfopen(nvr_concat("thor.nvr"),        "wb"); break;
                case ROM_MRTHOR:      f = romfopen(nvr_concat("mrthor.nvr"),      "wb"); break;
                case ROM_ZAPPA:       f = romfopen(nvr_concat("zappa.nvr"),       "wb"); break;
#if 0
                case ROM_CMDPC60:     f = romfopen(nvr_concat("cmdpc60.nvr"),     "wb"); break;                
#endif
                case ROM_S1668:       f = romfopen(nvr_concat("tpatx.nvr"),       "wb"); break;
                default: return;
        }
        fwrite(nvrram,128,1,f);
        fclose(f);
}

void nvr_init()
{
        io_sethandler(0x0070, 0x0002, readnvr, NULL, NULL, writenvr, NULL, NULL,  NULL);
        timer_add(nvr_rtc, &rtctime, TIMER_ALWAYS_ENABLED, NULL);
        timer_add(nvr_onesec, &nvr_onesec_time, TIMER_ALWAYS_ENABLED, NULL);
        timer_add(nvr_update_end, &nvr_update_end_count, &nvr_update_end_count, NULL);

}
