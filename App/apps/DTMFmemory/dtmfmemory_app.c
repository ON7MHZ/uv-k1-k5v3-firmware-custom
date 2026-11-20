/* DTMF MEMORY - 10 stored DTMF lines (max 15 tones each) with names, sent from
 * the radio.
 *
 * Built on the structure of the ACARS RX overlay app (same app_api_t, same
 * asset mechanism). No division, no string literals, no const tables: the
 * overlay is tiny and the resident helpers are not linked in.
 *
 * List mode:
 *   UP/DOWN        select a line          0-9   jump to line 1..10 (0 = line 10)
 *   MENU or PTT    send the selected line (the app keys the transmitter itself)
 *   *              edit the selected line (tones and name)
 *   F              cycle tone/pause timing (100/100, 50/50, 150/150 ms)
 *   EXIT           save and quit
 * Edit mode, tones field:
 *   0-9 * F(=#)    append that tone      MENU  toggle letters (keys 1-4 = A-D)
 *   DOWN           delete last tone      UP    switch to the name field
 *   EXIT           back to the list
 * Edit mode, name field (phone-style multi-tap, 1 s between letters):
 *   2-9            letters (ABC2 DEF3 ...)   0 = space/0   1 = 1 - .
 *   DOWN           delete last character     UP    switch to the tones field
 * While sending, EXIT aborts after the current tone.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../app_api.h"
#include "dtmfmemory_assets.h"

#define SLOTS       10u
#define MAXTONES    15u
#define NAME_LEN    10u
#define ROWS        5u

/* config bytes: selected line, timing preset, 10 lengths, 10 x 8 packed tone
 * bytes, then 10 names of NAME_LEN characters (0-terminated unless full) */
#define CFG_SEL     0u
#define CFG_TIME    1u
#define CFG_LEN_AT  2u
#define CFG_DATA    12u
#define CFG_NAMES   92u
#define CFG_SIZE    (CFG_NAMES+SLOTS*NAME_LEN)

#define LEAD_MS     150u    /* carrier up before the first tone */
#define LOOP_MS     50u
#define REFRESH     20u     /* redraw every second (battery) */
#define TAP_LOOPS   20u     /* multi-tap window: 1 s */
#define API_TXPREP  368u    /* api_size needed for tx_prepare */
#define NO_KEY      0xFFu

enum { M_NONE, M_SENDING, M_SENT, M_DENIED, M_EMPTY, M_ABORT };

static struct {
    uint8_t *cfg;
    const app_api_t *A;
    uint8_t prevKey, running, edit, field, letters, msg, redraw, tick;
    uint8_t tapKey, tapIdx, tapAge;
} g;

/* GCC may turn simple freestanding loops into these calls. */
void *memset(void *d,int c,size_t n){ uint8_t *p=d; while(n)p[--n]=(uint8_t)c; return d; }
void *memcpy(void *d,const void *s,size_t n){
    uint8_t *p=d; const uint8_t *q=s; while(n){ --n; p[n]=q[n]; } return d;
}

static char *put(char *o,const char *s){ while(*s)*o++=*s++; return o; }

/* ---- stored lines: 4 bits per tone (0-9, 10-13 = A-D, 14 = *, 15 = #) ---- */
static uint8_t lenOf(uint8_t s){ return g.cfg[CFG_LEN_AT+s]; }

static uint8_t getCode(uint8_t s,uint8_t i){
    uint8_t b=g.cfg[CFG_DATA+(s<<3)+(i>>1)];
    return (i&1u)?(uint8_t)(b>>4):(uint8_t)(b&15u);
}

static void setCode(uint8_t s,uint8_t i,uint8_t c){
    uint8_t *p=&g.cfg[CFG_DATA+(s<<3)+(i>>1)];
    if(i&1u) *p=(uint8_t)((*p&0x0Fu)|(c<<4));
    else     *p=(uint8_t)((*p&0xF0u)|c);
}

static char codeChar(uint8_t c){
    if(c<10u) return (char)('0'+c);
    if(c<14u) return (char)('A'+c-10u);
    return c==14u?'*':'#';
}

/* ---- stored names ---- */
static uint8_t *nameOf(uint8_t s){ return &g.cfg[CFG_NAMES+s*NAME_LEN]; }

static uint8_t nameLen(uint8_t s){
    uint8_t *p=nameOf(s);
    uint8_t n=0u;
    while(n<NAME_LEN&&p[n]) n++;
    return n;
}

static char *putCodes(char *o,uint8_t s){
    uint8_t n=lenOf(s);
    for(uint8_t i=0;i<n;i++) *o++=codeChar(getCode(s,i));
    return o;
}

static char *putName(char *o,uint8_t s){
    uint8_t *p=nameOf(s);
    uint8_t n=nameLen(s);
    for(uint8_t i=0;i<n;i++) *o++=(char)p[i];
    return o;
}

/* ---- DTMF matrix: rows 697/770/852/941 Hz, columns 1209/1336/1477/1633 Hz ---- */
static void matrix(uint8_t c,uint8_t *r,uint8_t *k){
    if(c==0u){ *r=3u; *k=1u; }
    else if(c<10u){
        uint8_t row=0u,n=(uint8_t)(c-1u);
        while(n>=3u){ n=(uint8_t)(n-3u); row++; }
        *r=row; *k=n;
    }else if(c<14u){ *r=(uint8_t)(c-10u); *k=3u; }
    else{ *r=3u; *k=(c==14u)?0u:2u; }
}

static uint16_t rowHz(uint8_t r){ return r==0u?697u:(r==1u?770u:(r==2u?852u:941u)); }
static uint16_t colHz(uint8_t k){ return k==0u?1209u:(k==1u?1336u:(k==2u?1477u:1633u)); }
/* BK4819 tone word: f * 10.32444 */
static uint16_t hzWord(uint16_t f){
    return (uint16_t)((((uint32_t)f*1353245u)+65536u)>>17);
}

static uint16_t toneMs(void){
    uint8_t p=g.cfg[CFG_TIME];
    return p==1u?50u:(p==2u?150u:100u);
}

/* ---- screen ---- */
static void draw(void){
    const app_api_t *A=g.A;
    char ui[UI_SIZE] __attribute__((aligned(4)));
    char text[40];
    uint8_t sel=g.cfg[CFG_SEL];

    A->asset_read(0,ui,UI_SIZE);
    A->display_clear();
    A->status_clear();
    A->print_inverse(ui+T_TITLE,2,0,true,true,(uint8_t)(2u+T_TITLE_CHARS*4u));
    A->draw_battery();

    if(!g.edit){
        uint8_t top=sel<2u?0u:(sel>7u?5u:(uint8_t)(sel-2u));
        for(uint8_t r=0;r<ROWS;r++){
            uint8_t s=(uint8_t)(top+r);
            char *o=text;
            *o++=(s==sel)?'>':' ';
            *o++=(s==9u)?'0':(char)('1'+s);
            *o++=':';
            if(nameLen(s)) o=putName(o,s);
            else o=putCodes(o,s);
            *o='\0';
            A->print_normal(text,0,0,r);
        }
        char *o=putCodes(text,sel);
        *o='\0';
        A->print_tiny(text,0,43,false,true);
        o=put(text,ui+T_HINT);
        o=A->append_u32(o,toneMs());
        *o++='/';
        o=A->append_u32(o,toneMs());
        *o='\0';
        A->print_tiny(text,0,49,false,true);
    }else{
        char *o=put(text,ui+T_EDIT);
        *o++=(sel==9u)?'0':(char)('1'+sel);
        *o='\0';
        A->print_bold(text,0,127,0);

        uint8_t nl=nameLen(sel);
        o=putName(text,sel);
        if(g.field&&nl<NAME_LEN) *o++='_';
        *o='\0';
        A->print_normal(text,0,0,1);

        uint8_t n=lenOf(sel);
        o=putCodes(text,sel);
        if(!g.field&&n<MAXTONES) *o++='_';
        *o='\0';
        A->print_normal(text,0,0,2);

        o=A->append_u32(text,g.field?nl:n);
        *o++='/';
        o=A->append_u32(o,g.field?NAME_LEN:MAXTONES);
        *o='\0';
        A->print_tiny(text,0,43,false,true);
        A->print_tiny(g.field?ui+T_NMODE:(g.letters?ui+T_LET:ui+T_NUM),60,43,false,true);
        A->print_tiny(g.field?ui+T_NHINT:ui+T_EHINT,0,49,false,true);
    }

    if(g.msg){
        const char *m=ui+T_SENT;
        if(g.msg==M_SENDING) m=ui+T_SENDING;
        else if(g.msg==M_DENIED) m=ui+T_DENIED;
        else if(g.msg==M_EMPTY) m=ui+T_EMPTY;
        else if(g.msg==M_ABORT) m=ui+T_ABORT;
        A->print_tiny(m,80,43,false,true);
    }
}

static void show(void){
    g.A->battery_sample();
    draw();
    g.A->blit_status();
    g.A->blit_full();
}

/* ---- transmit the selected line ----
 * Carrier up, MCW tone primed, second DTMF tone enabled on the BK4819, then
 * each tone is keyed with tx_mute while the carrier stays on. */
static void sendLine(void){
    const app_api_t *A=g.A;
    uint8_t s=g.cfg[CFG_SEL];
    uint8_t n=lenOf(s);
    if(!n){ g.msg=M_EMPTY; return; }
    if(A->tx_state()){ g.msg=M_DENIED; return; }

    g.msg=M_SENDING;
    show();

    uint16_t t=toneMs();
    uint8_t r,k;
    bool abort=false;

    matrix(getCode(s,0),&r,&k);
    if(A->api_size>=API_TXPREP) A->tx_prepare(); else A->tx_set_params();
    A->tx_tone(rowHz(r));
    {
        uint16_t v=A->bk_read(0x70u);          /* tone 1 on: copy its gain to tone 2 */
        A->bk_write(0x70u,(uint16_t)(v|0x0080u|((v>>8)&0x7Fu)));
    }
    A->tx_mute(true);
    A->tx_carrier(true);
    A->delay_ms(LEAD_MS);

    for(uint8_t i=0;i<n&&!abort;i++){
        matrix(getCode(s,i),&r,&k);
        A->bk_write(0x71u,hzWord(rowHz(r)));
        A->bk_write(0x72u,hzWord(colHz(k)));
        A->tx_mute(false);
        A->delay_ms(t);
        A->tx_mute(true);
        A->delay_ms(t);
        if(A->get_key()==APP_KEY_EXIT) abort=true;
    }

    A->tx_carrier(false);
    A->tx_end();
    g.msg=abort?M_ABORT:M_SENT;
}

/* ---- keys ---- */
static void listKey(uint8_t key){
    uint8_t s=g.cfg[CFG_SEL];
    if(key<=9u) g.cfg[CFG_SEL]=key?(uint8_t)(key-1u):9u;
    else if(key==APP_KEY_UP){ if(s) g.cfg[CFG_SEL]=(uint8_t)(s-1u); }
    else if(key==APP_KEY_DOWN){ if(s+1u<SLOTS) g.cfg[CFG_SEL]=(uint8_t)(s+1u); }
    else if(key==APP_KEY_MENU||key==APP_KEY_PTT){
        sendLine();
        g.prevKey=g.A->get_key();      /* a held key must be released first */
    }
    else if(key==APP_KEY_STAR){
        g.edit=1u; g.field=0u; g.letters=0u; g.tapKey=NO_KEY;
    }
    else if(key==APP_KEY_F){
        uint8_t p=(uint8_t)(g.cfg[CFG_TIME]+1u);
        g.cfg[CFG_TIME]=(p>2u)?0u:p;
    }
    else if(key==APP_KEY_EXIT) g.running=0u;
}

static void codeKey(uint8_t key){
    uint8_t s=g.cfg[CFG_SEL];
    uint8_t n=lenOf(s);
    int c=-1;
    if(key<=9u){
        if(!g.letters) c=key;
        else if(key>=1u&&key<=4u) c=9+key;
    }
    else if(key==APP_KEY_STAR) c=14;
    else if(key==APP_KEY_F) c=15;

    if(c>=0){
        if(n<MAXTONES){ setCode(s,n,(uint8_t)c); g.cfg[CFG_LEN_AT+s]=(uint8_t)(n+1u); }
    }
    else if(key==APP_KEY_MENU) g.letters^=1u;
    else if(key==APP_KEY_DOWN){ if(n) g.cfg[CFG_LEN_AT+s]=(uint8_t)(n-1u); }
}

/* Phone-style multi-tap: the same key again within one second replaces the
 * last character by the next one of that key. The groups live in the assets
 * (KEYMAP, 5 bytes per key, 0-padded). */
static void nameKey(uint8_t key){
    uint8_t s=g.cfg[CFG_SEL];
    uint8_t *p=nameOf(s);
    uint8_t n=nameLen(s);
    if(key==APP_KEY_DOWN){
        if(n) p[n-1u]=0u;
        g.tapKey=NO_KEY;
        return;
    }
    if(key>9u) return;
    uint8_t map[5];
    uint8_t m=0u;
    g.A->asset_read((uint16_t)(KEYMAP+key*5u),map,5);
    while(m<5u&&map[m]) m++;
    if(!m) return;
    if(g.tapKey==key&&n){
        g.tapIdx++;
        if(g.tapIdx>=m) g.tapIdx=0u;
        p[n-1u]=map[g.tapIdx];
    }else if(n<NAME_LEN){
        g.tapKey=key;
        g.tapIdx=0u;
        p[n]=map[0];
        if(n+1u<NAME_LEN) p[n+1u]=0u;
    }
    g.tapAge=0u;
}

static void editKey(uint8_t key){
    if(key==APP_KEY_UP){ g.field^=1u; g.letters=0u; g.tapKey=NO_KEY; return; }
    if(key==APP_KEY_EXIT){ g.edit=0u; g.field=0u; g.letters=0u; g.tapKey=NO_KEY; return; }
    if(g.field) nameKey(key); else codeKey(key);
}

static void handleKeys(void){
    uint8_t key=g.A->get_key();
    if(key==APP_KEY_WAKE){ g.redraw=1u; key=APP_KEY_INVALID; }
    if(key==APP_KEY_SAVER) key=APP_KEY_INVALID;
    bool rep=(key==APP_KEY_DOWN)||(key==APP_KEY_UP&&!g.edit);
    if(key==APP_KEY_INVALID||(key==g.prevKey&&!rep)){ g.prevKey=key; return; }
    g.prevKey=key;
    g.redraw=1u;
    g.msg=M_NONE;
    g.A->backlight_on();
    if(g.edit) editKey(key); else listKey(key);
}

__attribute__((section(".text.entry"),used))
void app_main(const app_api_t *api){
    uint8_t cfg[CFG_SIZE];
    memset(cfg,0,sizeof cfg);
    g.A=api;
    g.cfg=cfg;
    g.prevKey=APP_KEY_INVALID;
    g.tapKey=NO_KEY;

    api->cfg_load(cfg,CFG_SIZE);
    if(cfg[CFG_SEL]>=SLOTS) cfg[CFG_SEL]=0u;
    if(cfg[CFG_TIME]>2u) cfg[CFG_TIME]=0u;
    for(uint8_t s=0;s<SLOTS;s++){
        if(cfg[CFG_LEN_AT+s]>MAXTONES) cfg[CFG_LEN_AT+s]=0u;
        uint8_t *p=&cfg[CFG_NAMES+s*NAME_LEN];
        bool bad=false;
        for(uint8_t i=0;i<NAME_LEN;i++){
            if(p[i]<32u||p[i]>126u) bad=true;
            if(bad) p[i]=0u;
        }
    }

    api->backlight_on();
    g.running=1u;
    g.redraw=1u;
    while(g.running){
        handleKeys();
        api->backlight_update();
        if(g.tapKey!=NO_KEY&&++g.tapAge>=TAP_LOOPS) g.tapKey=NO_KEY;
        if(++g.tick>=REFRESH){ g.tick=0u; g.redraw=1u; }
        if(g.redraw){ g.redraw=0u; show(); }
        api->delay_ms(LOOP_MS);
    }
    api->cfg_save(cfg,CFG_SIZE);
}
