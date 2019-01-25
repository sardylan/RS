
/*
 * radiosondes RS41-SG(P)
 * author: zilog80
 *
 * compile:
 *     gcc rs41ptu.c -lm -o rs41ptu
 *     (includes bch_ecc.c)
 * usage:
 *     ./rs41ptu [options] audio.wav
 *       options:
 *            -v, -vx, -vv  (info, aux, info/conf)
 *            -r, --raw
 *            -i, --invert
 *            -b           (alt. Demod.)
 *            --crc        (check CRC)
 *            --ecc        (Reed-Solomon)
 *            --ptu        (temperature)
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef CYGWIN
  #include <fcntl.h>  // cygwin: _setmode()
  #include <io.h>
#endif


typedef unsigned char  ui8_t;
typedef unsigned short ui16_t;
typedef unsigned int   ui32_t;
typedef short i16_t;
typedef int   i32_t;


#include "bch_ecc.h"  // RS/ecc/


typedef struct {
    int typ;
    int msglen;
    int msgpos;
    int parpos;
    int hdrlen;
    int frmlen;
} rscfg_t;

rscfg_t cfg_rs41 = { 41, (320-56)/2, 56, 8, 8, 320};


typedef struct {
    int frnr;
    char id[9];
    int week; int gpssec;
    int jahr; int monat; int tag;
    int wday;
    int std; int min; float sek;
    double lat; double lon; double alt;
    double vN; double vE; double vU;
    double vH; double vD; double vD2;
    float T;
    ui32_t crc;
} gpx_t;

gpx_t gpx;

int option_verbose = 0,  // ausfuehrliche Anzeige
    option_raw = 0,      // rohe Frames
    option_inv = 0,      // invertiert Signal
    option_res = 0,      // genauere Bitmessung
    option_crc = 0,      // check CRC
    option_avg = 0,      // moving average
    option_b = 0,
    option_ecc = 0,      // Reed-Solomon ECC
    option_sat = 0,      // GPS sat data
    option_ptu = 0,
    option_len = 0,
    wavloaded = 0;
int wav_channel = 0;     // audio channel: left
int rawin = 0;


#define HEADOFS 24 // HEADOFS+HEADLEN <= 64
#define HEADLEN 32 // HEADOFS+HEADLEN mod 8 = 0
#define FRAMESTART ((HEADOFS+HEADLEN)/8)

/*               10      B6      CA      11      22      96      12      F8      */
char header[] = "0000100001101101010100111000100001000100011010010100100000011111";
char buf[HEADLEN+1] = "x";
int bufpos = -1;

#define NDATA_LEN 320                    // std framelen 320
#define XDATA_LEN 198
#define FRAME_LEN (NDATA_LEN+XDATA_LEN)  // max framelen 518
ui8_t //xframe[FRAME_LEN] = { 0x10, 0xB6, 0xCA, 0x11, 0x22, 0x96, 0x12, 0xF8},    = xorbyte( frame)
         frame[FRAME_LEN] = { 0x86, 0x35, 0xf4, 0x40, 0x93, 0xdf, 0x1a, 0x60}; // = xorbyte(xframe)

char buffer_rawin[3*FRAME_LEN+12]; //## rawin1: buffer_rawin[2*FRAME_LEN+12];
int frameofs = 0;

#define MASK_LEN 64
ui8_t mask[MASK_LEN] = { 0x96, 0x83, 0x3E, 0x51, 0xB1, 0x49, 0x08, 0x98,
                         0x32, 0x05, 0x59, 0x0E, 0xF9, 0x44, 0xC6, 0x26,
                         0x21, 0x60, 0xC2, 0xEA, 0x79, 0x5D, 0x6D, 0xA1,
                         0x54, 0x69, 0x47, 0x0C, 0xDC, 0xE8, 0x5C, 0xF1,
                         0xF7, 0x76, 0x82, 0x7F, 0x07, 0x99, 0xA2, 0x2C,
                         0x93, 0x7C, 0x30, 0x63, 0xF5, 0x10, 0x2E, 0x61,
                         0xD0, 0xBC, 0xB4, 0xB6, 0x06, 0xAA, 0xF4, 0x23,
                         0x78, 0x6E, 0x3B, 0xAE, 0xBF, 0x7B, 0x4C, 0xC1};
/* LFSR: ab i=8 (mod 64):
 * m[16+i] = m[i] ^ m[i+2] ^ m[i+4] ^ m[i+6]
 * ________________3205590EF944C6262160C2EA795D6DA15469470CDCE85CF1
 * F776827F0799A22C937C3063F5102E61D0BCB4B606AAF423786E3BAEBF7B4CC196833E51B1490898
 */

/* ------------------------------------------------------------------------------------ */

int Nvar = 0;
float *bufvar = NULL;
float xsum = 0,
      qsum = 0;
float mu, bvar[FRAME_LEN];

/* ------------------------------------------------------------------------------------ */

// option_b: exakte Baudrate wichtig!
// im Prinzip in sync-preamble/header ermittelbar
#define BAUD_RATE 4800

int sample_rate = 0, bits_sample = 0, channels = 0;
float samples_per_bit = 0;

int findstr(char *buff, char *str, int pos) {
    int i;
    for (i = 0; i < 4; i++) {
        if (buff[(pos+i)%4] != str[i]) break;
    }
    return i;
}

int read_wav_header(FILE *fp) {
    char txt[4+1] = "\0\0\0\0";
    unsigned char dat[4];
    int byte, p=0;

    if (fread(txt, 1, 4, fp) < 4) return -1;
    if (strncmp(txt, "RIFF", 4)) return -1;
    if (fread(txt, 1, 4, fp) < 4) return -1;
    // pos_WAVE = 8L
    if (fread(txt, 1, 4, fp) < 4) return -1;
    if (strncmp(txt, "WAVE", 4)) return -1;
    // pos_fmt = 12L
    for ( ; ; ) {
        if ( (byte=fgetc(fp)) == EOF ) return -1;
        txt[p % 4] = byte;
        p++; if (p==4) p=0;
        if (findstr(txt, "fmt ", p) == 4) break;
    }
    if (fread(dat, 1, 4, fp) < 4) return -1;
    if (fread(dat, 1, 2, fp) < 2) return -1;

    if (fread(dat, 1, 2, fp) < 2) return -1;
    channels = dat[0] + (dat[1] << 8);

    if (fread(dat, 1, 4, fp) < 4) return -1;
    memcpy(&sample_rate, dat, 4); //sample_rate = dat[0]|(dat[1]<<8)|(dat[2]<<16)|(dat[3]<<24);

    if (fread(dat, 1, 4, fp) < 4) return -1;
    if (fread(dat, 1, 2, fp) < 2) return -1;
    //byte = dat[0] + (dat[1] << 8);

    if (fread(dat, 1, 2, fp) < 2) return -1;
    bits_sample = dat[0] + (dat[1] << 8);

    // pos_dat = 36L + info
    for ( ; ; ) {
        if ( (byte=fgetc(fp)) == EOF ) return -1;
        txt[p % 4] = byte;
        p++; if (p==4) p=0;
        if (findstr(txt, "data", p) == 4) break;
    }
    if (fread(dat, 1, 4, fp) < 4) return -1;


    fprintf(stderr, "sample_rate: %d\n", sample_rate);
    fprintf(stderr, "bits       : %d\n", bits_sample);
    fprintf(stderr, "channels   : %d\n", channels);

    if ((bits_sample != 8) && (bits_sample != 16)) return -1;

    samples_per_bit = sample_rate/(float)BAUD_RATE;

    fprintf(stderr, "samples/bit: %.2f\n", samples_per_bit);

    return 0;
}


#define EOF_INT  0x1000000

#define LEN_movAvg 3
int movAvg[LEN_movAvg];
unsigned long sample_count = 0;

int read_signed_sample(FILE *fp) {  // int = i32_t
    int byte, i, sample=0, s=0;     // EOF -> 0x1000000
    float x=0, x0=0;

    for (i = 0; i < channels; i++) {
                           // i = 0: links bzw. mono
        byte = fgetc(fp);
        if (byte == EOF) return EOF_INT;
        if (i == wav_channel) sample = byte;

        if (bits_sample == 16) {
            byte = fgetc(fp);
            if (byte == EOF) return EOF_INT;
            if (i == wav_channel) sample +=  byte << 8;
        }

    }

    if (bits_sample ==  8)  s = sample-128;   // 8bit: 00..FF, centerpoint 0x80=128
    if (bits_sample == 16)  s = (short)sample;

    if (option_b)
    {
        x = s/128.0;
        if (bits_sample == 16) { x /= 256.0; }
        bufvar[sample_count % Nvar] = x;
        x0 = bufvar[(sample_count+1) % Nvar];
        xsum = xsum - x0 + x;
        qsum = qsum - x0*x0 + x*x;
    }

    if (option_avg) {
        movAvg[sample_count % LEN_movAvg] = s;
        s = 0;
        for (i = 0; i < LEN_movAvg; i++) s += movAvg[i];
        s = (s+0.5) / LEN_movAvg;
    }

    sample_count++;

    return s;
}

int par=1, par_alt=1;

int read_bits_fsk(FILE *fp, int *bit, int *len) {
    static int sample;
    int n, y0;
    float l, x1;
    static float x0;

    n = 0;
    do{
        y0 = sample;
        sample = read_signed_sample(fp);
        if (sample == EOF_INT) return EOF;
        //sample_count++; // in read_signed_sample()
        par_alt = par;
        par =  (sample >= 0) ? 1 : -1;  // 8bit: 0..127,128..255 (-128..-1,0..127)
        n++;
    } while (par*par_alt > 0);

    if (!option_res) l = (float)n / samples_per_bit;
    else {                                 // genauere Bitlaengen-Messung
        x1 = sample/(float)(sample-y0);    // hilft bei niedriger sample rate
        l = (n+x0-x1) / samples_per_bit;   // meist mehr frames (nicht immer)
        x0 = x1;
    }

    *len = (int)(l+0.5);

    if (!option_inv) *bit = (1+par_alt)/2;  // oben 1, unten -1
    else             *bit = (1-par_alt)/2;  // sdr#<rev1381?, invers: unten 1, oben -1

    /* Y-offset ? */

    return 0;
}

int bitstart = 0;
double bitgrenze = 0;
unsigned long scount = 0;
int read_rawbit(FILE *fp, int *bit) {
    int sample;
    int sum;
    int sample0, pars;

    sum = 0;

    sample0 = 0;
    pars = 0;

    if (bitstart) {
        scount = 1;    // (sample_count overflow/wrap-around)
        bitgrenze = 0; // d.h. bitgrenze = sample_count-1 (?)
        bitstart = 0;
    }
    bitgrenze += samples_per_bit;

    do {
        sample = read_signed_sample(fp);
        if (sample == EOF_INT) return EOF;
        //sample_count++; // in read_signed_sample()
        //par =  (sample >= 0) ? 1 : -1;    // 8bit: 0..127,128..255 (-128..-1,0..127)
        sum += sample;

        if (sample * sample0 < 0) pars++;   // wenn sample[0..n-1]=0 ...
        sample0 = sample;

        scount++;
    } while (scount < bitgrenze);  // n < samples_per_bit

    if (sum >= 0) *bit = 1;
    else          *bit = 0;

    if (option_inv) *bit ^= 1;

    return pars;
}


/* ------------------------------------------------------------------------------------ */


int bits2byte(char bits[]) {
    int i, byteval=0, d=1;
    for (i = 0; i < 8; i++) {     // little endian
    /* for (i = 7; i >= 0; i--) { // big endian */
        if      (bits[i] == 1)  byteval += d;
        else if (bits[i] == 0)  byteval += 0;
        else return 0x100;
        d <<= 1;
    }
    return byteval;
}


void inc_bufpos() {
  bufpos = (bufpos+1) % HEADLEN;
}

int compare() {
    int i=0, j = bufpos;

    while (i < HEADLEN) {
        if (j < 0) j = HEADLEN-1;
        if (buf[j] != header[HEADOFS+HEADLEN-1-i]) break;
        j--;
        i++;
    }
    return i;
}
/*
ui8_t xorbyte(int pos) {
    return  xframe[pos] ^ mask[pos % MASK_LEN];
}
*/
ui8_t framebyte(int pos) {
    return  frame[pos];
}


/* ------------------------------------------------------------------------------------ */
/*
 * Convert GPS Week and Seconds to Modified Julian Day.
 * - Adapted from sci.astro FAQ.
 * - Ignores UTC leap seconds.
 */
void Gps2Date(long GpsWeek, long GpsSeconds, int *Year, int *Month, int *Day) {

    long GpsDays, Mjd;
    long J, C, Y, M;

    GpsDays = GpsWeek * 7 + (GpsSeconds / 86400);
    Mjd = 44244 + GpsDays;

    J = Mjd + 2468570;
    C = 4 * J / 146097;
    J = J - (146097 * C + 3) / 4;
    Y = 4000 * (J + 1) / 1461001;
    J = J - 1461 * Y / 4 + 31;
    M = 80 * J / 2447;
    *Day = J - 2447 * M / 80;
    J = M / 11;
    *Month = M + 2 - (12 * J);
    *Year = 100 * (C - 49) + Y + J;
}
/* ------------------------------------------------------------------------------------ */

ui32_t u4(ui8_t *bytes) {  // 32bit unsigned int
    ui32_t val = 0;
    memcpy(&val, bytes, 4);
    return val;
}

ui32_t u3(ui8_t *bytes) {  // 24bit unsigned int
    int val24 = 0;
    val24 = bytes[0] | (bytes[1]<<8) | (bytes[2]<<16);
    // = memcpy(&val, bytes, 3), val &= 0x00FFFFFF;
    return val24;
}

int i3(ui8_t *bytes) {  // 24bit signed int
    int val = 0,
        val24 = 0;
    val = bytes[0] | (bytes[1]<<8) | (bytes[2]<<16);
    val24 = val & 0xFFFFFF; if (val24 & 0x800000) val24 = val24 - 0x1000000;
    return val24;
}

ui32_t u2(ui8_t *bytes) {  // 16bit unsigned int
    return  bytes[0] | (bytes[1]<<8);
}

/*
double r8(ui8_t *bytes) {
    double val = 0;
    memcpy(&val, bytes, 8);
    return val;
}

float r4(ui8_t *bytes) {
    float val = 0;
    memcpy(&val, bytes, 4);
    return val;
}
*/

/*
int crc16x(int start, int len) {
    int crc16poly = 0x1021;
    int rem = 0xFFFF, i, j;
    int xbyte;

    if (start+len+2 > FRAME_LEN) return -1;

    for (i = 0; i < len; i++) {
        xbyte = xorbyte(start+i);
        rem = rem ^ (xbyte << 8);
        for (j = 0; j < 8; j++) {
            if (rem & 0x8000) {
                rem = (rem << 1) ^ crc16poly;
            }
            else {
                rem = (rem << 1);
            }
            rem &= 0xFFFF;
        }
    }
    return rem;
}
*/
int crc16(int start, int len) {
    int crc16poly = 0x1021;
    int rem = 0xFFFF, i, j;
    int byte;

    if (start+len+2 > FRAME_LEN) return -1;

    for (i = 0; i < len; i++) {
        byte = framebyte(start+i);
        rem = rem ^ (byte << 8);
        for (j = 0; j < 8; j++) {
            if (rem & 0x8000) {
                rem = (rem << 1) ^ crc16poly;
            }
            else {
                rem = (rem << 1);
            }
            rem &= 0xFFFF;
        }
    }
    return rem;
}

int check_CRC(ui32_t pos, ui32_t pck) {
    ui32_t crclen = 0,
           crcdat = 0;
    if (((pck>>8) & 0xFF) != frame[pos]) return -1;
    crclen = frame[pos+1];
    if (pos + crclen + 4 > FRAME_LEN) return -1;
    crcdat = u2(frame+pos+2+crclen);
    if ( crcdat != crc16(pos+2, crclen) ) {
        return 1;  // CRC NO
    }
    else return 0; // CRC OK
}


/*
  Pos: SubHeader, 1+1 byte (ID+LEN)
0x039: 7928   FrameNumber+SondeID
              +(0x050: 0732  CalFrames 0x00..0x32)
0x065: 7A2A   PTU
0x093: 7C1E   GPS1: RXM-RAW (0x02 0x10) Week, TOW, Sats
0x0B5: 7D59   GPS2: RXM-RAW (0x02 0x10) pseudorange, doppler
0x112: 7B15   GPS3: NAV-SOL (0x01 0x06) ECEF-POS, ECEF-VEL
0x12B: 7611   00
0x12B: 7Exx   AUX-xdata
*/

#define crc_FRAME    (1<<0)
#define xor_FRAME    0x1713  // ^0x6E3B=0x7928
#define pck_FRAME    0x7928
#define pos_FRAME     0x039
#define pos_FrameNb   0x03B  // 2 byte
#define pos_SondeID   0x03D  // 8 byte
#define pos_CalData   0x052  // 1 byte, counter 0x00..0x32
#define pos_Calfreq   0x055  // 2 byte, calfr 0x00
#define pos_Calburst  0x05E  // 1 byte, calfr 0x02
// ? #define pos_Caltimer  0x05A  // 2 byte, calfr 0x02 ?
#define pos_CalRSTyp  0x05B  // 8 byte, calfr 0x21 (+2 byte in 0x22?)
        // weitere chars in calfr 0x22/0x23; weitere ID

#define crc_PTU      (1<<1)
#define pck_PTU      0x7A2A  // PTU
#define pos_PTU       0x065

#define crc_GPS1     (1<<2)
#define xor_GPS1     0x9667  // ^0xEA79=0x7C1E
#define pck_GPS1     0x7C1E  // RXM-RAW (0x02 0x10)
#define pos_GPS1      0x093
#define pos_GPSweek   0x095  // 2 byte
#define pos_GPSiTOW   0x097  // 4 byte
#define pos_satsN     0x09B  // 12x2 byte (1: SV, 1: quality,strength)

#define crc_GPS2     (1<<3)
#define pck_GPS2     0x7D59  // RXM-RAW (0x02 0x10)
#define pos_GPS2      0x0B5
#define pos_minPR     0x0B7  //        4 byte
#define pos_FF        0x0BB  //        1 byte
#define pos_dataSats  0x0BC  // 12x(4+3) byte (4: pseudorange, 3: doppler)

#define crc_GPS3     (1<<4)
#define xor_GPS3     0xB9FF  // ^0xC2EA=0x7B15
#define pck_GPS3     0x7B15  // NAV-SOL (0x01 0x06)
#define pos_GPS3      0x112
#define pos_GPSecefX  0x114  //   4 byte
#define pos_GPSecefY  0x118  //   4 byte
#define pos_GPSecefZ  0x11C  //   4 byte
#define pos_GPSecefV  0x120  // 3*2 byte
#define pos_numSats   0x126  //   1 byte
#define pos_sAcc      0x127  //   1 byte
#define pos_pDOP      0x128  //   1 byte

#define crc_AUX      (1<<5)
#define pck_AUX      0x7E00  // LEN variable
#define pos_AUX       0x12B

#define crc_ZERO     (1<<6)  // LEN variable
#define pck_ZERO     0x7600


ui8_t calibytes[51*16];
ui8_t calfrchk[51];
float Rf1,      // ref-resistor f1 (750 Ohm)
      Rf2,      // ref-resistor f2 (1100 Ohm)
      co1[3],   // { -243.911 , 0.187654 , 8.2e-06 }
      calT1[3], // calibration T1
      co2[3],   // { -243.911 , 0.187654 , 8.2e-06 }
      calT2[3]; // calibration T2-Hum


double c = 299.792458e6;
double L1 = 1575.42e6;

int get_SatData() {
    int i, n;
    int sv;
    ui32_t minPR;
    int Nfix;
    double pDOP, sAcc;

    fprintf(stdout, "[%d]\n", u2(frame+pos_FrameNb));

    fprintf(stdout, "iTOW: 0x%08X", u4(frame+pos_GPSiTOW));
    fprintf(stdout, "  week: 0x%04X", u2(frame+pos_GPSweek));
    fprintf(stdout, "\n");
    minPR = u4(frame+pos_minPR);
    fprintf(stdout, "minPR: %d", minPR);
    fprintf(stdout, "\n");

    for (i = 0; i < 12; i++) {
        n = i*7;
        sv = frame[pos_satsN+2*i];
        if (sv == 0xFF) break;
        fprintf(stdout, "    SV: %2d #  ", sv);
        fprintf(stdout, "prMes: %.1f", u4(frame+pos_dataSats+n)/100.0 + minPR);
        fprintf(stdout, "  ");
        fprintf(stdout, "doMes: %.1f", -i3(frame+pos_dataSats+n+4)/100.0*L1/c);
        fprintf(stdout, "\n");
    }

    fprintf(stdout, "ECEF-POS: (%d,%d,%d)\n",
                     (i32_t)u4(frame+pos_GPSecefX),
                     (i32_t)u4(frame+pos_GPSecefY),
                     (i32_t)u4(frame+pos_GPSecefZ));
    fprintf(stdout, "ECEF-VEL: (%d,%d,%d)\n",
                     (i16_t)u2(frame+pos_GPSecefV+0),
                     (i16_t)u2(frame+pos_GPSecefV+2),
                     (i16_t)u2(frame+pos_GPSecefV+4));

    Nfix = frame[pos_numSats];
    sAcc = frame[pos_sAcc]/10.0;
    pDOP = frame[pos_pDOP]/10.0;
    fprintf(stdout, "numSatsFix: %2d  sAcc: %.1f  pDOP: %.1f\n", Nfix, sAcc, pDOP);


    fprintf(stdout, "CRC: ");
    fprintf(stdout, " %04X", pck_GPS1);
    if (check_CRC(pos_GPS1, pck_GPS1)==0) fprintf(stdout, "[OK]"); else fprintf(stdout, "[NO]");
    //fprintf(stdout, "[%+d]", check_CRC(pos_GPS1, pck_GPS1));
    fprintf(stdout, " %04X", pck_GPS2);
    if (check_CRC(pos_GPS2, pck_GPS2)==0) fprintf(stdout, "[OK]"); else fprintf(stdout, "[NO]");
    //fprintf(stdout, "[%+d]", check_CRC(pos_GPS2, pck_GPS2));
    fprintf(stdout, " %04X", pck_GPS3);
    if (check_CRC(pos_GPS3, pck_GPS3)==0) fprintf(stdout, "[OK]"); else fprintf(stdout, "[NO]");
    //fprintf(stdout, "[%+d]", check_CRC(pos_GPS3, pck_GPS3));

    fprintf(stdout, "\n");
    fprintf(stdout, "\n");

    return 0;
}


int get_FrameNb() {
    int i;
    unsigned byte;
    ui8_t frnr_bytes[2];
    int frnr;

    for (i = 0; i < 2; i++) {
        byte = framebyte(pos_FrameNb + i);
        frnr_bytes[i] = byte;
    }

    frnr = frnr_bytes[0] + (frnr_bytes[1] << 8);
    gpx.frnr = frnr;

    return 0;
}

int get_SondeID(int crc) {
    int i;
    unsigned byte;
    char sondeid_bytes[9];

    if (crc == 0) {
        for (i = 0; i < 8; i++) {
            byte = framebyte(pos_SondeID + i);
            //if ((byte < 0x20) || (byte > 0x7E)) return -1;
            sondeid_bytes[i] = byte;
        }
        sondeid_bytes[8] = '\0';
        if ( strncmp(gpx.id, sondeid_bytes, 8) != 0 ) {
            //for (i = 0; i < 51; i++) calfrchk[i] = 0;
            memset(calfrchk, 0, 51);
            memcpy(gpx.id, sondeid_bytes, 8);
            gpx.id[8] = '\0';
        }
    }

    return 0;
}

int get_FrameConf() {
    int crc, err;
    ui8_t calfr;
    int i;

    crc = check_CRC(pos_FRAME, pck_FRAME);
    if (crc) gpx.crc |= crc_FRAME;

    err = crc;
    err |= get_FrameNb();
    err |= get_SondeID(crc);

    if (crc == 0) {
        calfr = framebyte(pos_CalData);
        if (calfrchk[calfr] == 0) // const?
        {                         // 0x32 not constant
            for (i = 0; i < 16; i++) {
                calibytes[calfr*16 + i] = framebyte(pos_CalData+1+i);
            }
            calfrchk[calfr] = 1;
        }
    }

    return err;
}

int get_CalData() {

    memcpy(&Rf1, calibytes+61, 4);  // 0x03*0x10+13
    memcpy(&Rf2, calibytes+65, 4);  // 0x04*0x10+ 1

    memcpy(co1+0, calibytes+77, 4);  // 0x04*0x10+13
    memcpy(co1+1, calibytes+81, 4);  // 0x05*0x10+ 1
    memcpy(co1+2, calibytes+85, 4);  // 0x05*0x10+ 5

    memcpy(calT1+0, calibytes+89, 4);  // 0x05*0x10+ 9
    memcpy(calT1+1, calibytes+93, 4);  // 0x05*0x10+13
    memcpy(calT1+2, calibytes+97, 4);  // 0x06*0x10+ 1

    memcpy(co2+0, calibytes+293, 4);  // 0x12*0x10+ 5
    memcpy(co2+1, calibytes+297, 4);  // 0x12*0x10+ 9
    memcpy(co2+2, calibytes+301, 4);  // 0x12*0x10+13

    memcpy(calT2+0, calibytes+305, 4);  // 0x13*0x10+ 1
    memcpy(calT2+1, calibytes+309, 4);  // 0x13*0x10+ 5
    memcpy(calT2+2, calibytes+313, 4);  // 0x13*0x10+ 9

    return 0;
}

float get_Tc0(ui32_t f, ui32_t f1, ui32_t f2) {
    // y  = (f - f1) / (f2 - f1);
    // y1 = (f - f1) / f2; // = (1 - f1/f2)*y
    float a =  3.9083e-3, // Pt1000 platinum resistance
          b = -5.775e-7,
          c = -4.183e-12; // below 0C, else C=0
    float *cal = calT1;
    float Rb = (f1*Rf2-f2*Rf1)/(f2-f1), // ofs
          Ra = f * (Rf2-Rf1)/(f2-f1) - Rb,
          raw = Ra/1000.0,
          g_r = 0.8024*cal[0] + 0.0176,  // empirisch
          r_o = 0.0705*cal[1] + 0.0011,  // empirisch
          r = raw * g_r + r_o,
          t = (-a + sqrt(a*a + 4*b*(r-1)))/(2*b); // t>0: c=0
    // R/R0 = 1 + at + bt^2 + c(t-100)t^3 , R0 = 1000 Ohm, t/Celsius
    return t;
}
float get_Tc(ui32_t f, ui32_t f1, ui32_t f2) {
    float *p = co1;
    float *c  = calT1;
    float  g = (float)(f2-f1)/(Rf2-Rf1),       // gain
          Rb = (f1*Rf2-f2*Rf1)/(float)(f2-f1), // ofs
          Rc = f/g - Rb,
          //R = (Rc + c[1]) * c[0],
          //T = p[0] + p[1]*R + p[2]*R*R;
          R = Rc * c[0],
          T = (p[0] + p[1]*R + p[2]*R*R + c[1])*(1.0 + c[2]);
    return T;
}

int get_PTU() {
    int err=0, i;
    int bR, bc1, bT1,
            bc2, bT2;
    ui32_t meas[12];
    float Tc = -273.15;
    float Tc0 = -273.15;

    get_CalData();

    err = check_CRC(pos_PTU, pck_PTU);
    if (err) gpx.crc |= crc_PTU;

    if (err == 0) {

        for (i = 0; i < 12; i++) {
            meas[i] = u3(frame+pos_PTU+2+3*i);
        }

        bR  = calfrchk[0x03] && calfrchk[0x04];
        bc1 = calfrchk[0x04] && calfrchk[0x05];
        bT1 = calfrchk[0x05] && calfrchk[0x06];
        bc2 = calfrchk[0x12] && calfrchk[0x13];
        bT2 = calfrchk[0x13];

        if (bR && bc1 && bT1) {
            Tc = get_Tc(meas[0], meas[1], meas[2]);
            Tc0 = get_Tc0(meas[0], meas[1], meas[2]);
        }
        gpx.T = Tc;

        if (option_verbose == 4)
        {
            printf("  h: %8.2f   # ", gpx.alt); // crc_GPS3 ?

            printf("1: %8d %8d %8d", meas[0], meas[1], meas[2]);
            printf("   #   ");
            printf("2: %8d %8d %8d", meas[3], meas[4], meas[5]);
            printf("   #   ");
            printf("3: %8d %8d %8d", meas[6], meas[7], meas[8]);
            printf("   #   ");
            if (Tc > -273.0) {
                printf("  T: %8.4f , T0: %8.4f ", Tc, Tc0);
            }
            printf("\n");

            if (gpx.alt > -100.0) {
                printf("    %9.2f ; %6.1f ; %6.1f ", gpx.alt, Rf1, Rf2);
                printf("; %10.6f ; %10.6f ; %10.6f ;", calT1[0], calT1[1], calT1[2]);
                printf("  %8d ; %8d ; %8d ", meas[0], meas[1], meas[2]);
                printf("; %10.6f ; %10.6f ; %10.6f ;", calT2[0], calT2[1], calT2[2]);
                printf("  %8d ; %8d ; %8d" , meas[6], meas[7], meas[8]);
                printf("\n");
            }
        }

    }

    return err;
}

int get_GPSweek() {
    int i;
    unsigned byte;
    ui8_t gpsweek_bytes[2];
    int gpsweek;

    for (i = 0; i < 2; i++) {
        byte = framebyte(pos_GPSweek + i);
        gpsweek_bytes[i] = byte;
    }

    gpsweek = gpsweek_bytes[0] + (gpsweek_bytes[1] << 8);
    //if (gpsweek < 0) { gpx.week = -1; return -1; } // (short int)
    gpx.week = gpsweek;

    return 0;
}

char weekday[7][3] = { "So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};

int get_GPStime() {
    int i;
    unsigned byte;
    ui8_t gpstime_bytes[4];
    int gpstime = 0, // 32bit
        day;
    int ms;

    for (i = 0; i < 4; i++) {
        byte = framebyte(pos_GPSiTOW + i);
        gpstime_bytes[i] = byte;
    }

    memcpy(&gpstime, gpstime_bytes, 4);
    ms = gpstime % 1000;
    gpstime /= 1000;

    gpx.gpssec = gpstime;

    day = (gpstime / (24 * 3600)) % 7;
    //if ((day < 0) || (day > 6)) return -1;  // besser CRC-check

    gpstime %= (24*3600);

    gpx.wday = day;
    gpx.std = gpstime / 3600;
    gpx.min = (gpstime % 3600) / 60;
    gpx.sek = gpstime % 60 + ms/1000.0;

    return 0;
}

int get_GPS1() {
    int err=0;

    // ((framebyte(pos_GPS1)<<8) | framebyte(pos_GPS1+1)) != pck_GPS1 ?
    if ( framebyte(pos_GPS1) != ((pck_GPS1>>8) & 0xFF) ) {
        gpx.crc |= crc_GPS1;
        return -1;
    }

    err = check_CRC(pos_GPS1, pck_GPS1);
    if (err) gpx.crc |= crc_GPS1;

    //err = 0;
    err |= get_GPSweek();
    err |= get_GPStime();

    return err;
}

int get_GPS2() {
    int err=0;

    err = check_CRC(pos_GPS2, pck_GPS2);
    if (err) gpx.crc |= crc_GPS2;

    return err;
}

#define EARTH_a  6378137.0
#define EARTH_b  6356752.31424518
#define EARTH_a2_b2  (EARTH_a*EARTH_a - EARTH_b*EARTH_b)

double a = EARTH_a,
       b = EARTH_b,
       a_b = EARTH_a2_b2,
       e2  = EARTH_a2_b2 / (EARTH_a*EARTH_a),
       ee2 = EARTH_a2_b2 / (EARTH_b*EARTH_b);

void ecef2elli(double X[], double *lat, double *lon, double *alt) {
    double phi, lam, R, p, t;

    lam = atan2( X[1] , X[0] );

    p = sqrt( X[0]*X[0] + X[1]*X[1] );
    t = atan2( X[2]*a , p*b );

    phi = atan2( X[2] + ee2 * b * sin(t)*sin(t)*sin(t) ,
                 p - e2 * a * cos(t)*cos(t)*cos(t) );

    R = a / sqrt( 1 - e2*sin(phi)*sin(phi) );
    *alt = p / cos(phi) - R;

    *lat = phi*180/M_PI;
    *lon = lam*180/M_PI;
}

int get_GPSkoord() {
    int i, k;
    unsigned byte;
    ui8_t XYZ_bytes[4];
    int XYZ; // 32bit
    double X[3], lat, lon, alt;
    ui8_t gpsVel_bytes[2];
    short vel16; // 16bit
    double V[3], phi, lam, dir;


    for (k = 0; k < 3; k++) {

        for (i = 0; i < 4; i++) {
            byte = frame[pos_GPSecefX + 4*k + i];
            XYZ_bytes[i] = byte;
        }
        memcpy(&XYZ, XYZ_bytes, 4);
        X[k] = XYZ / 100.0;

        for (i = 0; i < 2; i++) {
            byte = frame[pos_GPSecefV + 2*k + i];
            gpsVel_bytes[i] = byte;
        }
        vel16 = gpsVel_bytes[0] | gpsVel_bytes[1] << 8;
        V[k] = vel16 / 100.0;

    }


    // ECEF-Position
    ecef2elli(X, &lat, &lon, &alt);
    gpx.lat = lat;
    gpx.lon = lon;
    gpx.alt = alt;
    if ((alt < -1000) || (alt > 80000)) return -3;


    // ECEF-Velocities
    // ECEF-Vel -> NorthEastUp
    phi = lat*M_PI/180.0;
    lam = lon*M_PI/180.0;
    gpx.vN = -V[0]*sin(phi)*cos(lam) - V[1]*sin(phi)*sin(lam) + V[2]*cos(phi);
    gpx.vE = -V[0]*sin(lam) + V[1]*cos(lam);
    gpx.vU =  V[0]*cos(phi)*cos(lam) + V[1]*cos(phi)*sin(lam) + V[2]*sin(phi);

    // NEU -> HorDirVer
    gpx.vH = sqrt(gpx.vN*gpx.vN+gpx.vE*gpx.vE);
/*
    double alpha;
    alpha = atan2(gpx.vN, gpx.vE)*180/M_PI;  // ComplexPlane (von x-Achse nach links) - GeoMeteo (von y-Achse nach rechts)
    dir = 90-alpha;                          // z=x+iy= -> i*conj(z)=y+ix=re(i(pi/2-t)), Achsen und Drehsinn vertauscht
    if (dir < 0) dir += 360;                 // atan2(y,x)=atan(y/x)=pi/2-atan(x/y) , atan(1/t) = pi/2 - atan(t)
    gpx.vD2 = dir;
*/
    dir = atan2(gpx.vE, gpx.vN) * 180 / M_PI;
    if (dir < 0) dir += 360;
    gpx.vD = dir;

    return 0;
}

int get_GPS3() {
    int err=0;

    // ((framebyte(pos_GPS3)<<8) | framebyte(pos_GPS3+1)) != pck_GPS3 ?
    if ( framebyte(pos_GPS3) != ((pck_GPS3>>8) & 0xFF) ) {
        gpx.crc |= crc_GPS3;
        return -1;
    }

    err = check_CRC(pos_GPS3, pck_GPS3);
    if (err) gpx.crc |= crc_GPS3;

    err |= get_GPSkoord();

    return err;
}

int get_Aux() {
//
// "Ozone Sounding with Vaisala Radiosonde RS41" user's guide
//
    int i, auxlen, auxcrc, count7E, pos7E;

    count7E = 0;
    pos7E = pos_AUX;

    // 7Exx: xdata
    while ( pos7E < FRAME_LEN  &&  framebyte(pos7E) == 0x7E ) {

        auxlen = framebyte(pos7E+1);
        auxcrc = framebyte(pos7E+2+auxlen) | (framebyte(pos7E+2+auxlen+1)<<8);

        if ( auxcrc == crc16(pos7E+2, auxlen) ) {
            if (count7E == 0) fprintf(stdout, "\n # xdata = ");
            else              fprintf(stdout, " # ");

            //fprintf(stdout, " # %02x : ", framebyte(pos7E+2));
            for (i = 1; i < auxlen; i++) {
                fprintf(stdout, "%c", framebyte(pos7E+2+i));
            }
            count7E++;
            pos7E += 2+auxlen+2;
        }
        else {
            pos7E = FRAME_LEN;
            gpx.crc |= crc_AUX;
        }
    }

    i = check_CRC(pos7E, 0x7600);  // 0x76xx: 00-padding block
    if (i) gpx.crc |= crc_ZERO;

    return count7E;
}

int get_Calconf(int out) {
    int i;
    unsigned byte;
    ui8_t calfr = 0;
    ui8_t burst = 0;
    ui16_t fw = 0;
    int freq = 0, f0 = 0, f1 = 0;
    char sondetyp[9];
    int err = 0;

    byte = framebyte(pos_CalData);
    calfr = byte;
    err = check_CRC(pos_FRAME, pck_FRAME);

    if (option_verbose == 3) {
        fprintf(stdout, "\n");  // fflush(stdout);
        fprintf(stdout, "[%5d] ", gpx.frnr);
        fprintf(stdout, " 0x%02x: ", calfr);
        for (i = 0; i < 16; i++) {
            byte = framebyte(pos_CalData+1+i);
            fprintf(stdout, "%02x ", byte);
        }
        if (err == 0) fprintf(stdout, "[OK]");
        else          fprintf(stdout, "[NO]");
        fprintf(stdout, " ");
    }

    if (out)
    {
        if (calfr == 0x01  &&  option_verbose /*== 2*/) {
            fw = framebyte(pos_CalData+6) | (framebyte(pos_CalData+7)<<8);
            if (err == 0) fprintf(stdout, ": fw 0x%04x ", fw);
        }

        if (calfr == 0x02  &&  option_verbose /*== 2*/) {
            byte = framebyte(pos_Calburst);
            burst = byte;   // fw >= 0x4ef5, BK irrelevant? (burst-killtimer in 0x31?)
            if (err == 0) fprintf(stdout, ": BK %02X ", burst);
            if (err == 0 && option_verbose == 3) { // killtimer
                int kt = frame[0x5A] + (frame[0x5B] << 8); // short?
                if ( kt != 0xFFFF ) fprintf(stdout, ": kt 0x%04x = %dsec = %.1fmin ", kt, kt, kt/60.0);
            }
        }

        if (calfr == 0x00  &&  option_verbose) {
            byte = framebyte(pos_Calfreq) & 0xC0;  // erstmal nur oberste beiden bits
            f0 = (byte * 10) / 64;  // 0x80 -> 1/2, 0x40 -> 1/4 ; dann mal 40
            byte = framebyte(pos_Calfreq+1);
            f1 = 40 * byte;
            freq = 400000 + f1+f0; // kHz;
            if (err == 0) fprintf(stdout, ": fq %d ", freq);
        }

        if (calfr == 0x31  &&  option_verbose == 3) {
            if (err == 0) { // fw >= 0x4ef5: default=[88 77]=0x7788sec=510min
                int bt = frame[0x59] + (frame[0x5A] << 8); // short?
                if ( bt != 0x0000 ) fprintf(stdout, ": bt 0x%04x = %dsec = %.1fmin ", bt, bt, bt/60.0);
            }
        }

        if (calfr == 0x21  &&  option_verbose /*== 2*/) {  // eventuell noch zwei bytes in 0x22
            for (i = 0; i < 9; i++) sondetyp[i] = 0;
            for (i = 0; i < 8; i++) {
                byte = framebyte(pos_CalRSTyp + i);
                if ((byte >= 0x20) && (byte < 0x7F)) sondetyp[i] = byte;
                else if (byte == 0x00) sondetyp[i] = '\0';
            }
            if (err == 0) fprintf(stdout, ": %s ", sondetyp);
        }
    }

    return 0;
}

/*
  frame[pos_FRAME-1] == 0x0F: len == NDATA_LEN(320)
  frame[pos_FRAME-1] == 0xF0: len == FRAME_LEN(518)
*/
int frametype() { // -4..+4: 0xF0 -> -4 , 0x0F -> +4
    int i;
    ui8_t b = frame[pos_FRAME-1];
    int ft = 0;
    for (i = 0; i < 4; i++) {
        ft += ((b>>i)&1) - ((b>>(i+4))&1);
    }
    return ft;
}

/* ------------------------------------------------------------------------------------ */
/*
   (uses fec-lib by KA9Q)
   ka9q-fec:
      gcc -c init_rs_char.c
      gcc -c decode_rs_char.c

#include "fec.h"  // ka9q-fec


void *rs;
unsigned char codeword1[rs_N], codeword2[rs_N];

    rs = init_rs_char( 8, 0x11d, 0, 1, rs_R, 0);

    // ka9q-fec301: p(x) = p[0]x^(N-1) + ... + p[N-2]x + p[N-1]
    //          ->  cw[i] = codeword[RS.N-1-i]

*/

#define rs_N 255
#define rs_R 24
#define rs_K (rs_N-rs_R)

ui8_t cw1[rs_N], cw2[rs_N];

int rs41_ecc(int frmlen) {
// richtige framelen wichtig fuer 0-padding

    int i, leak, ret = 0;
    int errors1, errors2;
    ui8_t err_pos1[rs_R], err_pos2[rs_R],
          err_val1[rs_R], err_val2[rs_R];


    if (frmlen > FRAME_LEN) frmlen = FRAME_LEN;
    cfg_rs41.frmlen = frmlen;
    cfg_rs41.msglen = (frmlen-56)/2; // msgpos=56;
    leak = frmlen % 2;

    for (i = frmlen; i < FRAME_LEN; i++) frame[i] = 0;  // FRAME_LEN-HDR = 510 = 2*255


    for (i = 0; i < rs_R; i++) cw1[i] = frame[cfg_rs41.parpos+i     ];
    for (i = 0; i < rs_R; i++) cw2[i] = frame[cfg_rs41.parpos+i+rs_R];
    for (i = 0; i < rs_K; i++) cw1[rs_R+i] = frame[cfg_rs41.msgpos+2*i  ];
    for (i = 0; i < rs_K; i++) cw2[rs_R+i] = frame[cfg_rs41.msgpos+2*i+1];

    errors1 = rs_decode(cw1, err_pos1, err_val1);
    errors2 = rs_decode(cw2, err_pos2, err_val2);


    if (option_ecc == 2 && (errors1 < 0 || errors2 < 0)) {
        frame[pos_FRAME] = (pck_FRAME>>8)&0xFF; frame[pos_FRAME+1] = pck_FRAME&0xFF;
        frame[pos_PTU]   = (pck_PTU  >>8)&0xFF; frame[pos_PTU  +1] = pck_PTU  &0xFF;
        frame[pos_GPS1]  = (pck_GPS1 >>8)&0xFF; frame[pos_GPS1 +1] = pck_GPS1 &0xFF;
        frame[pos_GPS2]  = (pck_GPS2 >>8)&0xFF; frame[pos_GPS2 +1] = pck_GPS2 &0xFF;
        frame[pos_GPS3]  = (pck_GPS3 >>8)&0xFF; frame[pos_GPS3 +1] = pck_GPS3 &0xFF;
        if (frametype() < -2) {
            for (i = NDATA_LEN + 7; i < FRAME_LEN-2; i++) frame[i] = 0;
        }
        for (i = 0; i < rs_K; i++) cw1[rs_R+i] = frame[cfg_rs41.msgpos+2*i  ];
        for (i = 0; i < rs_K; i++) cw2[rs_R+i] = frame[cfg_rs41.msgpos+2*i+1];
        errors1 = rs_decode(cw1, err_pos1, err_val1);
        errors2 = rs_decode(cw2, err_pos2, err_val2);
    }


    // Wenn Fehler im 00-padding korrigiert wurden,
    // war entweder der frame zu kurz, oder
    // Fehler wurden falsch korrigiert;
    // allerdings ist bei t=12 die Wahrscheinlichkeit,
    // dass falsch korrigiert wurde mit 1/t! sehr gering.

    // check CRC32
    // CRC32 OK:
    //for (i = 0; i < cfg_rs41.hdrlen; i++) frame[i] = data[i];
    for (i = 0; i < rs_R; i++) {
        frame[cfg_rs41.parpos+     i] = cw1[i];
        frame[cfg_rs41.parpos+rs_R+i] = cw2[i];
    }
    for (i = 0; i < rs_K; i++) { // cfg_rs41.msglen <= rs_K
        frame[cfg_rs41.msgpos+  2*i] = cw1[rs_R+i];
        frame[cfg_rs41.msgpos+1+2*i] = cw2[rs_R+i];
    }
    if (leak) {
        frame[cfg_rs41.msgpos+2*i] = cw1[rs_R+i];
    }


    ret = errors1 + errors2;
    if (errors1 < 0 || errors2 < 0) {
        ret = 0;
        if (errors1 < 0) ret |= 0x1;
        if (errors2 < 0) ret |= 0x2;
        ret = -ret;
    }

    return ret;
}

/* ------------------------------------------------------------------------------------ */


int print_position(int ec) {
    int i;
    int err, err0, err1, err2, err3;
    int output, out_mask;

    err = get_FrameConf();

    err1 = get_GPS1();
    err2 = get_GPS2();
    err3 = get_GPS3();

    err0 = get_PTU();

    out_mask = crc_FRAME|crc_GPS1|crc_GPS3;
    output = ((gpx.crc & out_mask) != out_mask);  // (!err || !err1 || !err3);

    if (output) {

        if (!err) {
            fprintf(stdout, "[%5d] ", gpx.frnr);
            fprintf(stdout, "(%s) ", gpx.id);
        }
        if (!err1) {
            Gps2Date(gpx.week, gpx.gpssec, &gpx.jahr, &gpx.monat, &gpx.tag);
            fprintf(stdout, "%s ", weekday[gpx.wday]);
            fprintf(stdout, "%04d-%02d-%02d %02d:%02d:%06.3f",
                    gpx.jahr, gpx.monat, gpx.tag, gpx.std, gpx.min, gpx.sek);
            if (option_verbose == 3) fprintf(stdout, " (W %d)", gpx.week);
        }
        if (!err3) {
            fprintf(stdout, " ");
            fprintf(stdout, " lat: %.5f ", gpx.lat);
            fprintf(stdout, " lon: %.5f ", gpx.lon);
            fprintf(stdout, " alt: %.2f ", gpx.alt);
            //if (option_verbose)
            {
                //fprintf(stdout, "  (%.1f %.1f %.1f) ", gpx.vN, gpx.vE, gpx.vU);
                fprintf(stdout,"  vH: %4.1f  D: %5.1f°  vV: %3.1f ", gpx.vH, gpx.vD, gpx.vU);
            }
        }
        if (option_ptu && !err0) {
            if (gpx.T > -273.0) printf("  T=%.1fC ", gpx.T);
        }


        //if (output)
        {
            if (option_crc) {
                fprintf(stdout, " # ");
                if (option_ecc && ec >= 0 && (gpx.crc & 0x1F) != 0) {
                    int pos, blk, len, crc;   // unexpected blocks
                    int flen = NDATA_LEN;
                    if (frametype() < 0) flen += XDATA_LEN;
                    pos = pos_FRAME;
                    while (pos < flen-1) {
                        blk = frame[pos];     // 0x80XX: encrypted block
                        len = frame[pos+1];   // 0x76XX: 00-padding block
                        crc = check_CRC(pos, blk<<8);
                        fprintf(stdout, " %02X%02X", frame[pos], frame[pos+1]);
                        fprintf(stdout, "[%d]", crc&1);
                        pos = pos+2+len+2;
                    }
                }
                else {
                    fprintf(stdout, "[");
                    for (i=0; i<5; i++) fprintf(stdout, "%d", (gpx.crc>>i)&1);
                    fprintf(stdout, "]");
                }
                if (option_ecc == 2) {
                    if (ec > 0) fprintf(stdout, " (%d)", ec);
                    if (ec < 0) {
                        if      (ec == -1)  fprintf(stdout, " (-+)");
                        else if (ec == -2)  fprintf(stdout, " (+-)");
                        else   /*ec == -3*/ fprintf(stdout, " (--)");
                    }
                }
            }
        }

        get_Calconf(output);

        //if (output)
        {
            if (option_verbose > 1) get_Aux();
            fprintf(stdout, "\n");  // fflush(stdout);
        }
    }

    err |=  err1 | err3;

    return  err;
}

void print_frame(int len) {
    int i, ec = 0, ft;

    gpx.crc = 0;

    //frame[pos_FRAME-1] == 0x0F: len == NDATA_LEN(320)
    //frame[pos_FRAME-1] == 0xF0: len == FRAME_LEN(518)
    ft = frametype();
    if (ft > 2) len = NDATA_LEN;
    // STD-frames mit 00 auffuellen fuer Fehlerkorrektur
    if (len > NDATA_LEN  &&  len < NDATA_LEN+XDATA_LEN-10) {
        if (ft < -2) {
            len = NDATA_LEN + 7; // std-O3-AUX-frame
        }
    }
    // AUX-frames mit vielen Fehlern besser mit 00 auffuellen

    for (i = len; i < FRAME_LEN-2; i++) {
        frame[i] = 0;
    }
    if (ft > 2 || len == NDATA_LEN) {
        frame[FRAME_LEN-2] = 0;
        frame[FRAME_LEN-1] = 0;
    }
    if (len > NDATA_LEN) len = FRAME_LEN;
    else                 len = NDATA_LEN;


    if (option_ecc) {
        ec = rs41_ecc(len);
    }

    if (option_raw) {
/*
        for (i = 0; i < len; i++) {
            byte = framebyte(i);
            fprintf(stdout, "%02x", byte);
        }
        fprintf(stdout, "\n");
*/
        if (option_ecc == 2 && ec >= 0) {
            if (len < FRAME_LEN && frame[FRAME_LEN-1] != 0) len = FRAME_LEN;
        }
        for (i = 0; i < len; i++) {
            fprintf(stdout, "%02x", frame[i]);
        }
        if (option_ecc) {
            if (ec >= 0) fprintf(stdout, " [OK]"); else fprintf(stdout, " [NO]");
            if (option_ecc == 2 && ec > 0) fprintf(stdout, " (%d)", ec);
        }
        fprintf(stdout, "\n");
//        fprintf(stdout, "\n");
    }
    else if (option_sat) {
        get_SatData();
    }
    else {
        print_position(ec);
    }
}

int main(int argc, char *argv[]) {

    FILE *fp;
    char *fpname;
    char bitbuf[8];
    int bit_count = 0,
        byte_count = FRAMESTART,
        ft_len = FRAME_LEN,
        header_found = 0,
        byte, i;
    int bit, len,
        frmlen = FRAME_LEN;
    char *pbuf = NULL,
         *buf_sp = NULL;

    int sumQ, bitQ, Qerror_count;
    double ratioQ;

#ifdef CYGWIN
    _setmode(fileno(stdin), _O_BINARY);  // _fileno(stdin)
#endif
    setbuf(stdout, NULL);

    fpname = argv[0];
    ++argv;
    while ((*argv) && (!wavloaded)) {
        if      ( (strcmp(*argv, "-h") == 0) || (strcmp(*argv, "--help") == 0) ) {
            fprintf(stderr, "%s [options] audio.wav\n", fpname);
            fprintf(stderr, "  options:\n");
            fprintf(stderr, "       -v, -vx, -vv  (info, aux, info/conf)\n");
            fprintf(stderr, "       -r, --raw\n");
            fprintf(stderr, "       -i, --invert\n");
            //fprintf(stderr, "       --avg        (moving average)\n");
            fprintf(stderr, "       -b           (alt. Demod.)\n");
            fprintf(stderr, "       --crc        (check CRC)\n");
            fprintf(stderr, "       --ecc        (Reed-Solomon)\n");
            fprintf(stderr, "       --std        (std  framelen 320)\n");
            fprintf(stderr, "       --std2       (full framelen 518)\n");
            fprintf(stderr, "       --sat        (GPS Sat data)\n");
            fprintf(stderr, "       --ptu        (temperature)\n");
            return 0;
        }
        else if ( (strcmp(*argv, "-v") == 0) || (strcmp(*argv, "--verbose") == 0) ) {
            option_verbose = 1;
        }
        else if   (strcmp(*argv, "-vx") == 0) { option_verbose = 2; }
        else if   (strcmp(*argv, "-vv") == 0) { option_verbose = 3; }
        else if   (strcmp(*argv, "-vvv") == 0) { option_verbose = 4; }
        else if   (strcmp(*argv, "--crc") == 0) { option_crc = 1; }
        else if   (strcmp(*argv, "--res") == 0) { option_res = 1; }
        else if ( (strcmp(*argv, "-r") == 0) || (strcmp(*argv, "--raw") == 0) ) {
            option_raw = 1;
        }
        else if ( (strcmp(*argv, "-i") == 0) || (strcmp(*argv, "--invert") == 0) ) {
            option_inv = 1;
        }
        else if ( (strcmp(*argv, "--avg") == 0) ) {
            option_avg = 1;
        }
        else if   (strcmp(*argv, "-b") == 0) { option_b = 1; }
        else if   (strcmp(*argv, "--ecc" ) == 0) { option_ecc = 1; }
        else if   (strcmp(*argv, "--ecc2") == 0) { option_ecc = 2; }
        else if   (strcmp(*argv, "--std" ) == 0) { option_len = 1; frmlen = 320; }  // NDATA_LEN
        else if   (strcmp(*argv, "--std2") == 0) { option_len = 2; frmlen = 518; }  // NDATA_LEN+XDATA_LEN
        else if   (strcmp(*argv, "--sat") == 0) { option_sat = 1; }
        else if   (strcmp(*argv, "--ptu") == 0) { option_ptu = 1; }
        else if   (strcmp(*argv, "--ch2") == 0) { wav_channel = 1; }  // right channel (default: 0=left)
        else if   (strcmp(*argv, "--rawin1") == 0) { rawin = 2; }     // raw_txt input1
        else if   (strcmp(*argv, "--rawin2") == 0) { rawin = 3; }     // raw_txt input2
        else {
            fp = fopen(*argv, "rb");
            if (fp == NULL) {
                fprintf(stderr, "%s konnte nicht geoeffnet werden\n", *argv);
                return -1;
            }
            wavloaded = 1;
        }
        ++argv;
    }
    if (!wavloaded) fp = stdin;


    if (option_ecc) {
        rs_init_RS255();
    }


    if (!rawin) {

        i = read_wav_header(fp);
        if (i) {
            fclose(fp);
            return -1;
        }

        if (option_b)
        {
            Nvar = 32*samples_per_bit;
            bufvar  = (float *)calloc( Nvar+1, sizeof(float)); if (bufvar  == NULL) return -1;
            for (i = 0; i < Nvar; i++) bufvar[i] = 0;
        }

        while (!read_bits_fsk(fp, &bit, &len)) {

            if (len == 0) { // reset_frame();
                if (byte_count > pos_AUX) {
                    print_frame(byte_count);
                    bit_count = 0;
                    byte_count = FRAMESTART;
                    header_found = 0;
                }
                //inc_bufpos();
                //buf[bufpos] = 'x';
                continue;   // ...
            }

            for (i = 0; i < len; i++) {

                inc_bufpos();
                buf[bufpos] = 0x30 + bit;  // Ascii

                if (!header_found) {
                    if (compare() >= HEADLEN) header_found = 1;
                }
                else {
                    bitbuf[bit_count] = bit;
                    bit_count++;

                    if (bit_count == 8) {
                        bit_count = 0;
                        byte = bits2byte(bitbuf);
                        //xframe[byte_count] = byte;
                        frame[byte_count] = byte ^ mask[byte_count % MASK_LEN];
                        byte_count++;
                        if (byte_count == frmlen) {
                            byte_count = FRAMESTART;
                            header_found = 0;
                            print_frame(frmlen);
                        }
                    }
                }

            }
            if (header_found && option_b) {
                bitstart = 1;
                sumQ = 0;
                Qerror_count = 0;
                ft_len = frmlen;

                while ( byte_count < frmlen ) {
                    bitQ = read_rawbit(fp, &bit); // return: zeroX/bit (oder alternativ Varianz/bit)
                    if ( bitQ == EOF) break;
                    sumQ += bitQ; // zeroX/byte
                    bitbuf[bit_count] = bit;
                    bit_count++;
                    if (bit_count == 8) {
                        bit_count = 0;
                        byte = bits2byte(bitbuf);
                        //xframe[byte_count] = byte;
                        frame[byte_count] = byte ^ mask[byte_count % MASK_LEN];

                        mu = xsum/(float)Nvar;
                        bvar[byte_count] = qsum/(float)Nvar - mu*mu;

                        if (byte_count > NDATA_LEN) {  // Fehler erst ab minimaler framelen Zaehlen
                            //ratioQ = sumQ/samples_per_bit; // approx: bei Rauschen zeroX/byte leider nicht linear in sample_rate
                            //if (ratioQ > 0.7) {            // sr=48k: 0.7, Schwelle, ab wann wahrscheinlich Rauschbit
                            if (bvar[byte_count]*2 > bvar[byte_count-300]*3) { // Var(frame)/Var(noise) ca. 1:2
                                Qerror_count += 1;
                            }
                        }
                        sumQ = 0; // Fenster fuer zeroXcount: 8 bit

                        byte_count++;
                    }
                    if (Qerror_count == 4 && option_len == 0) { // framelen = 320 oder 518
                        ft_len = byte_count;
                        Qerror_count += 1;
                    }
                }
                header_found = 0;
                print_frame(ft_len);
                byte_count = FRAMESTART;
            }

        }

        if (option_b)
        {
            if (bufvar)  { free(bufvar); bufvar = NULL; }
        }

    }
    else //if (rawin)
    {
        if (rawin == 3) frameofs = 8;
        else            frameofs = 0;

        while (1 > 0) {

            pbuf = fgets(buffer_rawin, rawin*FRAME_LEN+12, fp);
            if (pbuf == NULL) break;
            buffer_rawin[rawin*FRAME_LEN] = '\0';
            if (rawin == 2) {
                buf_sp = strchr(buffer_rawin, ' ');
                if (buf_sp != NULL && buf_sp-buffer_rawin < rawin*FRAME_LEN) {
                    buffer_rawin[buf_sp-buffer_rawin] = '\0';
                }
            }
            len = strlen(buffer_rawin) / rawin;
            if (len > pos_SondeID+10) {
                for (i = 0; i < len; i++) { //%2x  SCNx8=%hhx(inttypes.h)
                    sscanf(buffer_rawin+rawin*i, "%2hhx", frame+frameofs+i);
                    // wenn ohne %hhx: sscanf(buffer_rawin+rawin*i, "%2x", &byte); frame[frameofs+i] = (ui8_t)byte;
                }
                if (rawin == 3) {
                    len += frameofs;
                    if ((frame[NDATA_LEN-1]<<8)+frame[NDATA_LEN-2] == 0xc7ec) len = NDATA_LEN; //**//
                }
                print_frame(len);
            }
        }
    }

    fclose(fp);

    return 0;
}

