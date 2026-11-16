// Copyright 2024 George Norton (@george-norton)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#define MXT336UD_ADDRESS (0x4A << 1)
#define MXT_I2C_TIMEOUT_MS 20
#define MXT_REG_INFORMATION_BLOCK (0)

// Object table entries cannot be read individually, we have to read starting at the
// beginning of an object. For now we will allocate a large object on the stack, but
// a general purpose I2C buffer might be a better approach if other interesting objects
// are large.

// TODO: The address bytes are reversed - can probably read them individuallys
#define MXT_MAX_OBJECTS 64

typedef struct PACKED {
    unsigned char type;
    unsigned char position_ls_byte;
    unsigned char position_ms_byte;
    unsigned char size_minus_one;
    unsigned char instances_minus_one;
    unsigned char report_ids_per_instance;
} mxt_object_table_element;

typedef struct PACKED {
    unsigned char family_id;
    unsigned char variant_id;
    unsigned char version;
    unsigned char build;
    unsigned char matrix_x_size;
    unsigned char matrix_y_size;
    unsigned char num_objects;
    // mxt_object_table_element objects[MXT_MAX_OBJECTS];
} mxt_information_block;

typedef struct PACKED {
    unsigned char report_id;
#ifdef DIGITIZER_REPORT_FINGER_SIZE
    unsigned char data[8];
#else
    unsigned char data[6];
#endif
} mxt_message;

typedef struct PACKED {
    unsigned char count;
} mxt_message_count;

typedef struct PACKED {
    unsigned short status;
    unsigned char  payloadcrc[3];
    unsigned char  enccustcrc[3];
    unsigned char  error;
} mxt_gen_encryptionstatus_t2;

typedef struct PACKED {
    unsigned char reset;
    unsigned char backupnv;
    unsigned char calibrate;
    unsigned char reportall;
    unsigned char debugctrl;
    unsigned char diagnostic;
    unsigned char debugctrl2;
} mxt_gen_commandprocessor_t6;

static const unsigned char T6_DIAGNOSTIC_PAGE_UP                           = 0x01;
static const unsigned char T6_DIAGNOSTIC_PAGE_DOWN                         = 0x02;
static const unsigned char T6_DIAGNOSTIC_MUTUAL_CAPACITANCE_DELTA_MODE     = 0x10;
static const unsigned char T6_DIAGNOSTIC_MUTUAL_CAPACITANCE_REFERENCE_MODE = 0x11;
static const unsigned char T6_DIAGNOSTIC_MUTUAL_CAPACITANCE_DC_DATA_MODE   = 0x38;
static const unsigned char T6_DIAGNOSTIC_DEVICE_INFORMATION_MODE           = 0x80;
static const unsigned char T6_DIAGNOSTIC_SELF_CAPACITANCE_SIGNAL_MODE      = 0xF5;
static const unsigned char T6_DIAGNOSTIC_SELF_CAPACITANCE_DELTA_MODE       = 0xF7;
static const unsigned char T6_DIAGNOSTIC_SELF_CAPACITANCE_REFERENCE_MODE   = 0xF8;

static const unsigned char T6_DIAGNOSTIC_KEY_DELTA_MODE                        = 0x17;
static const unsigned char T6_DIAGNOSTIC_KEY_REFERENCE_MODE                    = 0x18;
static const unsigned char T6_DIAGNOSTIC_KEY_SIGNAL_MODE                       = 0x19;
static const unsigned char T6_DIAGNOSTIC_CALIBRATION_RECOVERY_TUNING_DATA_MODE = 0x33;
static const unsigned char T6_DIAGNOSTIC_PIN_FAULT_SELF_TEST_MODE              = 0x35;
static const unsigned char T6_DIAGNOSTIC_TOUCH_STATUS_DATA_MODE                = 0x36;
static const unsigned char T6_DIAGNOSTIC_PRODUCT_DATA_STORE_MODE               = 0x81;
static const unsigned char T6_DIAGNOSTIC_TOUCHSCREEN_MODE                      = 0xF4;

typedef struct PACKED {
    unsigned char idleacqint;
    unsigned char actacqint;
    unsigned char actv2idelto;
    unsigned char cfg;
    unsigned char cfg2;
    unsigned char idleacqintfine;
    unsigned char actvaqintfine;
} mxt_gen_powerconfig_t7;

static const unsigned char T7_CFG_INITACTV             = 0x80;
static const unsigned char T7_CFG_OVFRPTSUP            = 0x40;
static const unsigned char T7_CFG_ACTV2IDLETOMSB_SHIFT = 2;
static const unsigned char T7_CFG_ACTV2IDLETOMSB_MASK  = 0x3C;
static const unsigned char T7_CFG_ACTVPIPEEN           = 0x2;
static const unsigned char T7_CFG_IDLEPIPEEN           = 0x1;

typedef struct PACKED {
    unsigned char chrgtime;
    unsigned char reserved;
    unsigned char tchdrift;
    unsigned char driftst;
    unsigned char tchautocal;
    unsigned char sync;
    unsigned char atchcalst;
    unsigned char atchcalsthr;
    unsigned char atchfrccalthr;
    unsigned char atchfrccalratio;
    unsigned char measallow;
    unsigned char reserved2[3];
    unsigned char cfg;
} mxt_gen_acquisitionconfig_t8;


typedef struct PACKED {
    unsigned char ctrl;
    unsigned char cmd;
    unsigned char upsiglim_lsb;
    unsigned char upsiglim_msb;
    unsigned char losiglim_lsb;
    unsigned char losiglim_msb;
    unsigned char pindwellus;
    unsigned char sigrangelim_lsb;
    unsigned char sigrangelim_msb;
    unsigned char pinthr;
    unsigned char pertstinterval;
    unsigned char pertstholdoff;
    unsigned char pertstrptfactor;
    unsigned char pertstrtpwidth;
    unsigned char pertstcfg;
    unsigned char sesiglimits[3];
} mxt_spt_selftest_t25;

static const unsigned char T25_TEST_FINISHED     = 0x00;
static const unsigned char T25_TEST_POWER        = 0x01;
static const unsigned char T25_TEST_PIN_FAULT    = 0x12;
static const unsigned char T25_TEST_SIGNAL_LIMIT = 0x17;
static const unsigned char T25_TEST_ALL          = 0xFE;
static const unsigned char T25_TEST_INVALID      = 0xFD;
static const unsigned char T25_TEST_PASSED       = 0xFE;

typedef struct PACKED {
    unsigned char ctrl;
    unsigned char reserved;
    unsigned char maxapprarea;
    unsigned char maxtcharea;
    unsigned char supstrength;
    unsigned char supextto;
    unsigned char maxnumtchs;
    unsigned char shapestrength;
    unsigned char supdist;
    unsigned char disthyst;
    unsigned char maxscrnarea;
    unsigned char cfg;
    unsigned char reserved2;
    unsigned char edgesupstrength;
} mxt_proci_touchsupression_t42;


static const unsigned char T42_CTRL_ENABLE    = 0x01;
static const unsigned char T42_CTRL_SHAPEEN   = 0x04;
static const unsigned char T42_CTRL_DISLOBJ   = 0x08;
static const unsigned char T42_CTRL_DISTLOCK  = 0x10;
static const unsigned char T42_CTRL_SUPDISTEN = 0x20;
static const unsigned char T42_CTRL_EDGESUP   = 0x40;

static const unsigned char T42_CFG_RELAXCLOSEUP = 0x01;
static const unsigned char T42_CFG_RELAXDIAGSUP = 0x02;
static const unsigned char T42_CFG_SUPTCHRPTEN  = 0x04;

typedef struct PACKED {
    unsigned char  reserved[2];
    unsigned char  idlesyncsperx;
    unsigned char  activesyncsperx;
    unsigned char  adcspersync;
    unsigned char  pulsesperadc;
    unsigned char  xslew;
    unsigned short syncdelay;
    unsigned char  xvoltage;
    unsigned char  reserved2;
    unsigned char  inrushcfg;
    unsigned char  reserved3[6];
    unsigned char  cfg;
} mxt_spt_cteconfig_t46;

static const unsigned char T46_CFG_SYNCLOSSMODE = 0x02;
static const unsigned char T46_CFG_DELTASHIFT   = 0x04;


typedef struct PACKED {
    unsigned char ctrl;
    unsigned char reserved;
    unsigned char contmax;
    unsigned char stability;
    unsigned char maxtcharea;
    unsigned char amplthr;
    unsigned char styshape;
    unsigned char hoversup;
    unsigned char confthr;
    unsigned char syncsperx;
    unsigned char xposadj;
    unsigned char yposadj;
    unsigned char cfg;
    unsigned char reserved2[7];
    unsigned char supstyto;
    unsigned char maxnumsty;
    unsigned char xedgectrl;
    unsigned char xedgedist;
    unsigned char yedgectrl;
    unsigned char yedgedist;
    unsigned char supto;
    unsigned char supclassmode;
    unsigned char dxxedgectrl;
    unsigned char dxxedgedist;
    unsigned char xedgectrlhi;
    unsigned char xedgedisthi;
    unsigned char dxxedgectrlhi;
    unsigned char dxxedgedisthi;
    unsigned char yedgectrlhi;
    unsigned char yedgedisthi;
    unsigned char cfg2;
    unsigned char movfilter;
    unsigned char movsmooth;
    unsigned char movpred;
    unsigned char satbxlo;
    unsigned char satbxhi;
    unsigned char satbylo;
    unsigned char satbyhi;
    unsigned char satbdxxlo;
    unsigned char satbdxxhi;
    unsigned char movhistcfg;
} mxt_proci_stylus_t47;


static const unsigned char T47_CFG_SUPSTY = 0x20;

typedef struct PACKED {
    unsigned char ctrl;
    unsigned char reserved;
    unsigned char optint;
    unsigned char inttime;
    unsigned char intdelay[41];
} mxt_proci_shieldless_t56;


static const unsigned char T56_CTRL_ENABLE = 0x01;
static const unsigned char T56_CTRL_RPTEN  = 0x02;

typedef struct PACKED {
    unsigned char ctrl;
    unsigned char gradthr;
    unsigned char ylonoisemul_lsb;
    unsigned char ylonoisemul_msb;
    unsigned char ylonoisediv_lsb;
    unsigned char ylonoisediv_msb;
    unsigned char yhinoisemul_lsb;
    unsigned char yhinoisemul_msb;
    unsigned char yhinoisediv_lsb;
    unsigned char yhinoisediv_msb;
    unsigned char lpfiltcoef;
    unsigned char forcescale_lsb;
    unsigned char forcescale_msb;
    unsigned char forcethr;
    unsigned char forcethrhyst;
    unsigned char forcedi;
    unsigned char forcehyst;
    unsigned char atchratio;
    unsigned char reserved[2];
    unsigned char exfrcthr;
    unsigned char exfrcthrhyst;
    unsigned char exfrcto;
} mxt_proci_lensbending_t65;

static const unsigned char T65_CTRL_ENABLE         = 0x01;
static const unsigned char T65_CTRL_RPTEN          = 0x02;
static const unsigned char T65_CTRL_DISPRESS       = 0x08;
static const unsigned char T65_CTRL_DISRELEASE     = 0x10;
static const unsigned char T65_CTRL_DISHIST        = 0x80;
static const unsigned char T65_ATCHRATIO_ATCHRATIO = 0x01;
static const unsigned char T65_ATCHRATIO_DSRATIO   = 0x10;

typedef struct PACKED {
    unsigned char ctrl;
    unsigned char compgain;
    unsigned char targetdelta;
    unsigned char compthr;
    unsigned char atchthr;
    unsigned char moistcfg;
    unsigned char reserved;
    unsigned char moistthr;
    unsigned char moistinvtchthr;
    unsigned char moistcfg2;
    unsigned char compstrthr;
    unsigned char compcfg;
    unsigned char moistvldthrsf;
    unsigned char moistcfg3;
    unsigned char moistdegthr;
} mxt_proci_retransmissioncompensation_t80;

typedef struct PACKED {
    unsigned char  ctrl;
    unsigned char  cfg1;
    unsigned char  scraux;
    unsigned char  tchaux;
    unsigned char  tcheventcfg;
    unsigned char  akscfg;
    unsigned char  numtch;
    unsigned char  xycfg;
    unsigned char  xorigin;
    unsigned char  xsize;
    unsigned char  xpitch;
    unsigned char  xlocip;
    unsigned char  xhiclip;
    unsigned short xrange;
    unsigned char  xedgecfg;
    unsigned char  xedgedist;
    unsigned char  dxxedgecfg;
    unsigned char  dxxedgedist;
    unsigned char  yorigin;
    unsigned char  ysize;
    unsigned char  ypitch;
    unsigned char  ylocip;
    unsigned char  yhiclip;
    unsigned short yrange;
    unsigned char  yedgecfg;
    unsigned char  yedgedist;
    unsigned char  gain;
    unsigned char  dxgain;
    unsigned char  tchthr;
    unsigned char  tchhyst;
    unsigned char  intthr;
    unsigned char  noisesf;
    unsigned char  cutoffthr;
    unsigned char  mrgthr;
    unsigned char  mrgthradjstr;
    unsigned char  mrghyst;
    unsigned char  dxthrsf;
    unsigned char  tchdidown;
    unsigned char  tchdiup;
    unsigned char  nexttchdi;
    unsigned char  calcfg;
    unsigned char  jumplimit;
    unsigned char  movfilter;
    unsigned char  movsmooth;
    unsigned char  movpred;
    unsigned short movhysti;
    unsigned short movhystn;
    unsigned char  amplhyst;
    unsigned char  scrareahyst;
    unsigned char  intthryst;
    unsigned char  xedgecfghi;
    unsigned char  xedgedisthi;
    unsigned char  dxxedgecfghi;
    unsigned char  dxxedgedisthi;
    unsigned char  yedgecfghi;
    unsigned char  yedgedisthi;
    unsigned char  cfg2;
    unsigned char  movhystcfg;
    unsigned char  amplcoeff;
    unsigned char  amploffset;
    unsigned char  jumplimitmov;
    unsigned short jlmmovthr;
    unsigned char  jlmmovintthr;
} mxt_touch_multiscreen_t100;

static const unsigned char T100_CTRL_SCANEN     = 0x80;
static const unsigned char T100_CTRL_DISSCRMSG0 = 0x4;
static const unsigned char T100_CTRL_RPTEN      = 0x2;
static const unsigned char T100_CTRL_ENABLE     = 0x1;

static const unsigned char T100_CFG_INVERTX      = 0x80;
static const unsigned char T100_CFG_INVERTY      = 0x40;
static const unsigned char T100_CFG_SWITCHXY     = 0x20;
static const unsigned char T100_CFG_DISLOCK      = 0x10;
static const unsigned char T100_CFG_ATCHTHRSEL   = 0x8;
static const unsigned char T100_CFG_RPTEACHCYCLE = 0x1;

// Touch events reported in the t100 messages
enum { MXT_NO_EVENT, MXT_MOVE, MXT_UNSUP, MXT_SUP, MXT_DOWN, MXT_UP, MXT_UNSUPSUP, MXT_UNSUPUP, MXT_DOWNSUP, MXT_DOWNUP };

enum { MXT_FINGER = 1, MXT_PASSIVE_STYLUS, MXT_GLOVE = 5, MXT_LARGE_TOUCH };
