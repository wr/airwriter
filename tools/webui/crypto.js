/* W-1156: SHA-256, HMAC and PBKDF2 for signing in. The page is plain http,
   where browsers offer no crypto.subtle, so it carries its own. The board's
   core (auth_core.c) and test_crypto.mjs hold both to the same values. */
var AW_IV=[0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19];
var AW_K=new Int32Array([0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2]);
/* One block: H (8 words) takes in W[0..15]; W[16..63] is scratch. The round
   constants are the closure's, not a global's: a global is looked up on
   every one of the 64 rounds, and that was most of the time. */
var awCompress=(function(K){ return function(H,W){
  var i,x,y,a=H[0],b=H[1],c=H[2],d=H[3],e=H[4],f=H[5],g=H[6],h=H[7],t1,t2;
  for(i=16;i<64;i++){ x=W[i-15]; y=W[i-2];
    W[i]=(((x>>>7|x<<25)^(x>>>18|x<<14)^(x>>>3))+W[i-7]+((y>>>17|y<<15)^(y>>>19|y<<13)^(y>>>10))+W[i-16])|0; }
  for(i=0;i<64;i++){
    t1=(h+((e>>>6|e<<26)^(e>>>11|e<<21)^(e>>>25|e<<7))+((e&f)^(~e&g))+K[i]+W[i])|0;
    t2=(((a>>>2|a<<30)^(a>>>13|a<<19)^(a>>>22|a<<10))+((a&b)^(a&c)^(b&c)))|0;
    h=g; g=f; f=e; e=(d+t1)|0; d=c; c=b; b=a; a=(t1+t2)|0;
  }
  H[0]=H[0]+a|0; H[1]=H[1]+b|0; H[2]=H[2]+c|0; H[3]=H[3]+d|0;
  H[4]=H[4]+e|0; H[5]=H[5]+f|0; H[6]=H[6]+g|0; H[7]=H[7]+h|0;
}; })(AW_K);
function awBytes(H){ var o=new Uint8Array(32); for(var i=0;i<8;i++){ o[4*i]=H[i]>>>24; o[4*i+1]=H[i]>>>16; o[4*i+2]=H[i]>>>8; o[4*i+3]=H[i]; } return o; }
function awWords(b,off,W){ for(var i=0;i<16;i++) W[i]=b[off+4*i]<<24|b[off+4*i+1]<<16|b[off+4*i+2]<<8|b[off+4*i+3]; }
function awSha256(m){
  var n=m.length, len=((n+9+63)>>6)<<6, b=new Uint8Array(len), W=new Int32Array(64), H=new Int32Array(AW_IV), bits=n*8;
  b.set(m); b[n]=0x80;
  b[len-5]=bits/4294967296; b[len-4]=bits>>>24; b[len-3]=bits>>>16; b[len-2]=bits>>>8; b[len-1]=bits;
  for(var o=0;o<len;o+=64){ awWords(b,o,W); awCompress(H,W); }
  return awBytes(H);
}
function awJoin(){ var n=0,i,o,k=0; for(i=0;i<arguments.length;i++) n+=arguments[i].length;
  o=new Uint8Array(n); for(i=0;i<arguments.length;i++){ o.set(arguments[i],k); k+=arguments[i].length; } return o; }
function awHmac(k,m){
  if(k.length>64) k=awSha256(k);
  var ip=new Uint8Array(64), op=new Uint8Array(64);
  for(var i=0;i<64;i++){ ip[i]=(k[i]||0)^0x36; op[i]=(k[i]||0)^0x5c; }
  return awSha256(awJoin(op,awSha256(awJoin(ip,m))));
}
/* One 32-byte block. After U1, each round is two compressions of a single
   padded block from the pads' saved states, so the rounds stay quick. */
function awPbkdf2(pw,salt,iter){
  var k=pw.length>64?awSha256(pw):pw, W=new Int32Array(64), I=new Int32Array(AW_IV), O=new Int32Array(AW_IV),
      pad=new Uint8Array(64), i, c, U=new Int32Array(8), T=new Int32Array(8), S=new Int32Array(8), u;
  for(i=0;i<64;i++) pad[i]=(k[i]||0)^0x36; awWords(pad,0,W); awCompress(I,W);
  for(i=0;i<64;i++) pad[i]=(k[i]||0)^0x5c; awWords(pad,0,W); awCompress(O,W);
  u=awHmac(pw,awJoin(salt,new Uint8Array([0,0,0,1])));
  for(i=0;i<8;i++) U[i]=T[i]=u[4*i]<<24|u[4*i+1]<<16|u[4*i+2]<<8|u[4*i+3];
  for(c=1;c<iter;c++){
    S.set(I); for(i=0;i<8;i++) W[i]=U[i]; W[8]=0x80000000; for(i=9;i<15;i++) W[i]=0; W[15]=768; awCompress(S,W);
    U.set(O); for(i=0;i<8;i++) W[i]=S[i]; W[8]=0x80000000; for(i=9;i<15;i++) W[i]=0; W[15]=768; awCompress(U,W);
    for(i=0;i<8;i++) T[i]^=U[i];
  }
  return awBytes(T);
}
function awHex(b){ var s=''; for(var i=0;i<b.length;i++) s+=(b[i]<16?'0':'')+b[i].toString(16); return s; }
function awUnhex(s){ var o=new Uint8Array(s.length>>1); for(var i=0;i<o.length;i++) o[i]=parseInt(s.substr(2*i,2),16); return o; }
function awUtf8(s){ return new TextEncoder().encode(s); }
/* The sign-in's answer, and a new password sealed with the current one's key. */
function awSignProof(K,what,nonce){ return awHex(awHmac(K,awJoin(awUtf8(what),awUnhex(nonce)))); }
function awChangeSeal(K,nonce,pw){
  var n=awUnhex(nonce), s=awJoin(awHmac(K,awJoin(awUtf8('change'),n,new Uint8Array([1]))),
                               awHmac(K,awJoin(awUtf8('change'),n,new Uint8Array([2])))),
      p=awUtf8(pw), o=new Uint8Array(p.length);
  for(var i=0;i<p.length;i++) o[i]=p[i]^s[i];
  return awHex(o);
}
