// SPDX-License-Identifier: GPL-3.0-or-later
// The page's hashes (W-1156) against the values the board's core is held to
// (tools/rip/test/test_auth.c), all made with Python's hashlib.
//   node tools/webui/test_crypto.mjs
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const ctx = { TextEncoder };
vm.createContext(ctx);
vm.runInContext(readFileSync(new URL('./crypto.js', import.meta.url), 'utf8'), ctx);
const { awSha256, awHmac, awPbkdf2, awHex, awUnhex, awUtf8, awSignProof, awChangeSeal } = ctx;

let checks = 0, failed = 0;
function eq(got, want, what) {
  checks++;
  if (got !== want) { failed++; console.error(`FAIL ${what}: got ${got}, want ${want}`); }
}

eq(awHex(awSha256(awUtf8('abc'))),
   'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad', 'sha256 abc');
eq(awHex(awSha256(awUtf8('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq'))),
   '248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1', 'sha256 two blocks');
eq(awHex(awSha256(new Uint8Array(0))),
   'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855', 'sha256 empty');
eq(awHex(awHmac(new Uint8Array(20).fill(0x0b), awUtf8('Hi There'))),
   'b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7', 'hmac rfc4231 1');
for (const [c, dk] of [
  [1, '120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b'],
  [2, 'ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43'],
  [4096, 'c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a'],
]) eq(awHex(awPbkdf2(awUtf8('password'), awUtf8('salt'), c)), dk, `pbkdf2 c=${c}`);

// The sign-in and the change, as the page sends them.
const salt = awHex(Uint8Array.from({ length: 16 }, (_, i) => i));
const nonce = '00112233445566778899aabbccddeeff';
const K = awPbkdf2(awUtf8('k7m2x9qp'), awUnhex(salt), 1000);
eq(awHex(K), '0fdc64748538ed622377390df1a9dee40bc40b26399f794dcae012613e371826', 'K');
eq(awSignProof(K, 'signin', nonce),
   '08b2b19ea45f330916decd12523dc41f3e9876564aa6d08dc96ec4cc0e1763ac', 'proof');
eq(awChangeSeal(K, nonce, 'new-password-1'), 'ecac32bd55dfe0e2784e171a1a87', 'change seal');
eq(awUnhex('0aFf').join(','), '10,255', 'unhex');

const t0 = performance.now();
awPbkdf2(awUtf8('k7m2x9qp'), awUnhex(salt), 100000);
const ms = Math.round(performance.now() - t0);

if (failed) { console.error(`${failed}/${checks} checks FAILED`); process.exit(1); }
console.log(`ok  test_crypto.mjs  (${checks} checks; PBKDF2 at 100000 rounds: ${ms} ms here)`);
