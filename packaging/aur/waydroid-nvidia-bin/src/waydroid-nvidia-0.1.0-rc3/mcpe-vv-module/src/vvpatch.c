/* vvpatch — unlock Minecraft Bedrock "Vibrant Visuals" on any GPU.
 *
 * Usage: vvpatch <in.apk> <out.apk> <gl_renderer_string>
 *
 * Reads assets/assets/tiers.bin (base64 JSON {"gpu":{name:{tier,hash}}}),
 * sets every device tier to 5 (unlocks VV for GPUs already in Mojang's list),
 * and injects entries for the running GPU's GL_RENDERER string plus its
 * parenthesised sub-strings, each with the CORRECT tier-hash so RenderDragon's
 * lookup matches. The hash function (base-257 poly x (2^31+1)) is derived
 * self-calibrating from an existing table entry, so it keeps working across
 * Minecraft updates. Writes a repacked APK to out.apk (caller installs it
 * in place, preserving mtime, so PackageManager does not re-verify).
 */
#include "miniz.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define M31 0x80000000u          /* 2^31 */
#define MASK31 0x7fffffffu
#define TIERS_PATH "assets/assets/tiers.bin"

/* ---- base64 ---- */
static const char B64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int b64val(int c){
  if(c>='A'&&c<='Z')return c-'A';
  if(c>='a'&&c<='z')return c-'a'+26;
  if(c>='0'&&c<='9')return c-'0'+52;
  if(c=='+')return 62; if(c=='/')return 63; return -1;
}
static unsigned char *b64decode(const char*in,size_t inlen,size_t*outlen){
  unsigned char*out=malloc(inlen/4*3+4); size_t o=0; int q[4],n=0;
  for(size_t i=0;i<inlen;i++){int v=b64val((unsigned char)in[i]); if(v<0)continue;
    q[n++]=v; if(n==4){ out[o++]=(q[0]<<2)|(q[1]>>4); out[o++]=(q[1]<<4)|(q[2]>>2);
      out[o++]=(q[2]<<6)|q[3]; n=0; } }
  if(n==2){ out[o++]=(q[0]<<2)|(q[1]>>4); }
  else if(n==3){ out[o++]=(q[0]<<2)|(q[1]>>4); out[o++]=(q[1]<<4)|(q[2]>>2); }
  *outlen=o; return out;
}
static char *b64encode(const unsigned char*in,size_t len,size_t*outlen){
  char*out=malloc((len+2)/3*4+4); size_t o=0; size_t i=0;
  for(;i+3<=len;i+=3){ unsigned x=(in[i]<<16)|(in[i+1]<<8)|in[i+2];
    out[o++]=B64[(x>>18)&63]; out[o++]=B64[(x>>12)&63];
    out[o++]=B64[(x>>6)&63]; out[o++]=B64[x&63]; }
  size_t rem=len-i;
  if(rem==1){ unsigned x=in[i]<<16; out[o++]=B64[(x>>18)&63]; out[o++]=B64[(x>>12)&63];
    out[o++]='='; out[o++]='='; }
  else if(rem==2){ unsigned x=(in[i]<<16)|(in[i+1]<<8); out[o++]=B64[(x>>18)&63];
    out[o++]=B64[(x>>12)&63]; out[o++]=B64[(x>>6)&63]; out[o++]='='; }
  *outlen=o; return out;
}

/* ---- cracked tier-hash ---- */
/* Pf(s) = sum ord(c_i)*257^i mod 2^31 */
static uint32_t Pf(const char*s,size_t n){
  uint64_t acc=0, p=1;
  for(size_t i=0;i<n;i++){ acc=(acc + (uint64_t)(unsigned char)s[i]*p)%M31; p=(p*257)%M31; }
  return (uint32_t)acc;
}
static uint32_t modinv257; /* inverse of 257 mod 2^31 */
static uint32_t mulmod31(uint32_t a,uint32_t b){ return (uint32_t)(((uint64_t)a*b)%M31); }
static void init_modinv(void){
  /* extended euclid for 257 mod 2^31 */
  int64_t t=0,newt=1,r=M31,newr=257;
  while(newr){ int64_t q=r/newr;
    int64_t tmp=t-q*newt; t=newt; newt=tmp;
    tmp=r-q*newr; r=newr; newr=tmp; }
  if(t<0) t+=M31; modinv257=(uint32_t)t;
}
/* R(L) recurrence R(L)=257*R(L-1)+1 mod 2^31; anchored from a known entry */
static uint32_t R0; /* R at length 0 */
static void calibrate(const char*name,size_t nlen,uint32_t hash){
  /* R(nlen) = (hash&MASK31 - Pf(name)) mod 2^31 */
  uint32_t Rn=( (hash&MASK31) + M31 - Pf(name,nlen) )%M31;
  /* step down to R0: R(L-1)=(R(L)-1)*inv257 */
  for(size_t L=nlen; L>0; L--){ Rn=mulmod31( (Rn+M31-1)%M31, modinv257); }
  R0=Rn;
}
static uint32_t Rlen(size_t L){ uint32_t r=R0;
  for(size_t i=0;i<L;i++) r=(uint32_t)(((uint64_t)r*257 + 1)%M31); return r; }
static void hash_candidates(const char*s,uint32_t out[2]){
  size_t n=strlen(s);
  uint32_t lo=( (uint64_t)Pf(s,n)+Rlen(n) )%M31;
  out[0]=lo; out[1]=lo|M31;
}

/* ---- tiny growable buffer ---- */
typedef struct { char*p; size_t len, cap; } buf;
static void bput(buf*b,const char*s,size_t n){
  if(b->len+n+1>b->cap){ b->cap=(b->len+n+1)*2; b->p=realloc(b->p,b->cap); }
  memcpy(b->p+b->len,s,n); b->len+=n; b->p[b->len]=0;
}
static void bputs(buf*b,const char*s){ bput(b,s,strlen(s)); }

int main(int argc,char**argv){
  if(argc<4){ fprintf(stderr,"usage: %s in.apk out.apk gl_renderer\n",argv[0]); return 2; }
  const char*inapk=argv[1],*outapk=argv[2],*renderer=argv[3];
  init_modinv();

  /* read tiers.bin from apk */
  mz_zip_archive zin; memset(&zin,0,sizeof zin);
  if(!mz_zip_reader_init_file(&zin,inapk,0)){ fprintf(stderr,"open %s failed\n",inapk); return 1; }
  size_t rawlen=0;
  char*raw=mz_zip_reader_extract_file_to_heap(&zin,TIERS_PATH,&rawlen,0);
  if(!raw){ fprintf(stderr,"no %s in apk\n",TIERS_PATH); return 1; }
  size_t jlen=0; unsigned char*json=b64decode(raw,rawlen,&jlen);
  mz_free(raw);

  /* find "gpu" object start and an anchor entry (first name+hash) */
  char*txt=(char*)json; txt[jlen]=0;
  char*gpu=strstr(txt,"\"gpu\""); if(!gpu){ fprintf(stderr,"no gpu obj\n"); return 1; }
  char*brace=strchr(gpu,'{'); if(!brace){ fprintf(stderr,"no {\n"); return 1; }
  /* anchor: first "<name>" after brace, then its "hash": N */
  char*q1=strchr(brace,'\"'); char*q2=q1?strchr(q1+1,'\"'):0;
  char*hk=q2?strstr(q2,"\"hash\""):0;
  if(!q1||!q2||!hk){ fprintf(stderr,"parse anchor failed\n"); return 1; }
  size_t anlen=q2-(q1+1); char aname[256];
  if(anlen>=sizeof aname) anlen=sizeof aname-1;
  memcpy(aname,q1+1,anlen); aname[anlen]=0;
  uint32_t ahash=(uint32_t)strtoul(hk+7,0,10);
  calibrate(aname,strlen(aname),ahash);

  /* verify calibration against the anchor */
  { uint32_t c[2]; hash_candidates(aname,c);
    if((c[0]!=(ahash&MASK31))){ fprintf(stderr,"WARN calibration mismatch %u vs %u\n",c[0],ahash&MASK31); } }

  /* set every "tier": d -> 5  (single digit 1..5) */
  for(char*p=txt; (p=strstr(p,"\"tier\":")); ){
    char*d=p+7; while(*d==' '||*d=='\t')d++;
    if(*d>='1'&&*d<='5') *d='5';
    p=d;
  }

  /* Build candidate key strings. RenderDragon on ANGLE performs SEVERAL tier
   * lookups for one GPU (empirically: the full GL_RENDERER, the Vulkan device
   * name, the mesa "<name> (%s)" template, the driver version, the vendor —
   * VV needs ALL of them at tier>=4). We over-generate to be safe & universal:
   *   - full renderer
   *   - every parenthesised substring (nested), plus each with a leading word
   *     stripped once/twice
   *   - for every "PREFIX (CONTENT)" occurrence, also "PREFIX (%s)"
   *   - the tail after the last ", " (driver, e.g. "venus-26.0.65.35")
   *   - the first token (vendor, e.g. "NVIDIA")
   * On a native device the renderer == device name, so this still just works. */
  buf inj={0};
  #define MAXK 200
  char keys[MAXK][512]; int nk=0;
  #define ADDK(s) do{ const char*_s=(s); if(_s&&*_s&&strlen(_s)<511&&nk<MAXK){ int _d=0; for(int _k=0;_k<nk;_k++) if(!strcmp(keys[_k],_s)){_d=1;break;} if(!_d) strcpy(keys[nk++],_s);} }while(0)
  char tmp[512];
  size_t rl=strlen(renderer);
  /* base set: full renderer + every parenthesised substring (nested) */
  ADDK(renderer);
  for(size_t i=0;i<rl;i++) if(renderer[i]=='('){
    int depth=0; size_t j=i;
    for(;j<rl;j++){ if(renderer[j]=='(')depth++; else if(renderer[j]==')'){depth--; if(depth==0)break;} }
    if(j<rl && j>i+1){ size_t L=j-(i+1); if(L<511){ memcpy(tmp,renderer+i+1,L); tmp[L]=0; ADDK(tmp); } }
  }
  /* driver (tail after last ", ", strip trailing ')') + vendor (first token) */
  { const char*d=0; for(size_t i=0;i+1<rl;i++) if(renderer[i]==','&&renderer[i+1]==' ') d=renderer+i+2;
    if(d){ strncpy(tmp,d,511); tmp[511]=0; size_t L=strlen(tmp); while(L>0&&(tmp[L-1]==')'||tmp[L-1]==' ')) tmp[--L]=0; ADDK(tmp); } }
  { const char*v=renderer; if(!strncmp(v,"ANGLE (",7)) v+=7; strncpy(tmp,v,511); tmp[511]=0;
    char*e=tmp; while(*e&&*e!=','&&*e!=' '&&*e!='(') e++; *e=0; ADDK(tmp); }
  /* Expand each candidate with transforms until fixpoint (bounded):
   *  (a) strip leading word ("NVIDIA Virtio-GPU..." -> "Virtio-GPU...")
   *  (b) strip trailing " (...)"  ("X (A) (B)" -> "X (A)")
   *  (c) replace trailing " (...)" with " (%s)" (mesa template form)         */
  for(int pass=0; pass<4; pass++){
    int base=nk;
    for(int k=0;k<base;k++){
      const char*s=keys[k]; size_t L=strlen(s);
      /* (a) leading word */
      const char*sp=strchr(s,' '); if(sp && sp[1]) ADDK(sp+1);
      /* find trailing " (...)" : last " (" with matching ')' at end */
      if(L>=4 && s[L-1]==')'){
        int depth=0; long q=-1;
        for(long i=(long)L-1;i>=0;i--){ if(s[i]==')')depth++; else if(s[i]=='('){depth--; if(depth==0){q=i;break;}} }
        if(q>0 && s[q-1]==' '){
          /* (b) strip */
          memcpy(tmp,s,q-1); tmp[q-1]=0; ADDK(tmp);
          /* (c) (%s) form */
          memcpy(tmp,s,q); tmp[q]=0; strcat(tmp,"(%s)"); ADDK(tmp);
        }
      }
    }
    if(nk==base) break;
  }
  /* Emit each candidate hash under a UNIQUE synthetic key. RenderDragon looks
   * up the tier by matching the hash VALUE it computes for its GL_RENDERER
   * against the table, so the key name is cosmetic — but keys must be distinct
   * or a JSON parser would dedup and drop a candidate. We add both top-bit
   * variants for the full renderer and every parenthesised substring. */
  int vvid=0;
  for(int i=0;i<nk;i++){
    int dup=0; for(int k=0;k<i;k++) if(!strcmp(keys[i],keys[k])){dup=1;break;} if(dup)continue;
    uint32_t h[2]; hash_candidates(keys[i],h);
    for(int t=0;t<2;t++){ char line[128];
      int n=snprintf(line,sizeof line,"\"__vv%d\":{\"tier\":5,\"hash\":%u},\r\n",vvid++,h[t]);
      bput(&inj,line,n); }
  }

  /* splice injection right after the gpu '{' */
  size_t pre=(size_t)(brace+1-txt);
  buf outj={0};
  bput(&outj,txt,pre);
  bput(&outj,inj.p,inj.len);
  bput(&outj,txt+pre,jlen-pre);

  /* re-encode base64 */
  size_t enclen=0; char*enc=b64encode((unsigned char*)outj.p,outj.len,&enclen);

  /* repack apk: copy all entries except tiers.bin, add patched tiers.bin */
  mz_zip_archive zout; memset(&zout,0,sizeof zout);
  if(!mz_zip_writer_init_file(&zout,outapk,0)){ fprintf(stderr,"create %s failed\n",outapk); return 1; }
  mz_uint n=mz_zip_reader_get_num_files(&zin);
  for(mz_uint i=0;i<n;i++){
    char nm[1024]; mz_zip_reader_get_filename(&zin,i,nm,sizeof nm);
    if(!strcmp(nm,TIERS_PATH)) continue;
    if(!mz_zip_writer_add_from_zip_reader(&zout,&zin,i)){
      fprintf(stderr,"copy entry %s failed\n",nm); return 1; }
  }
  if(!mz_zip_writer_add_mem(&zout,TIERS_PATH,enc,enclen,MZ_DEFAULT_LEVEL)){
    fprintf(stderr,"add tiers failed\n"); return 1; }
  if(!mz_zip_writer_finalize_archive(&zout)){ fprintf(stderr,"finalize failed\n"); return 1; }
  mz_zip_writer_end(&zout);
  mz_zip_reader_end(&zin);
  fprintf(stderr,"vvpatch: %d keys, tiers->5, wrote %s\n",nk,outapk);
  return 0;
}
