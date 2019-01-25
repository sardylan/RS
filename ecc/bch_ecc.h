#ifndef __ECC_BCH_ECC_H
#define __ECC_BCH_ECC_H

typedef unsigned char  ui8_t;
typedef unsigned int  ui32_t;

int rs_init_RS255(void);

int rs_init_BCH64(void);

int rs_encode(ui8_t cw[]);

int rs_decode(ui8_t cw[], ui8_t *err_pos, ui8_t *err_val);

int rs_decode_ErrEra(ui8_t cw[], int nera, ui8_t era_pos[], ui8_t *err_pos, ui8_t *err_val);

int rs_decode_bch_gf2t2(ui8_t cw[], ui8_t *err_pos, ui8_t *err_val);

#endif
