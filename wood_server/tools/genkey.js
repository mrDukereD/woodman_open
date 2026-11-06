// Ключевая пара для подписи архивов обновления (RSA-2048). В exe попадает только открытый ключ (BCRYPT_RSAPUBLIC_BLOB).
//   node genkey.js [путь\к\release_private.pem]
// По умолчанию закрытый ключ пишется в <корень репозитория>\keys\release_private.pem (папка keys\ в .gitignore, ключ НЕ публиковать
// и хранить копию отдельно), открытый - в wood_server\pubkey_release.h.
// Уже существующий закрытый ключ НЕ перезаписывается (смена ключа = все выданные подписи и собранные exe перестают сходиться):
// для замены удалить файл руками.
'use strict';
const fs = require('fs'), path = require('path'), crypto = require('crypto');
const repo = path.resolve(__dirname, '..', '..');
const privFile = process.argv[2] || path.join(repo, 'keys', 'release_private.pem');
const hdrPath = path.join(repo, 'wood_server', 'pubkey_release.h');

let priv;
if (fs.existsSync(privFile)) { priv = crypto.createPrivateKey(fs.readFileSync(privFile)); console.log('закрытый ключ уже есть, оставляю:', privFile); }
else {
  const kp = crypto.generateKeyPairSync('rsa', { modulusLength: 2048, publicExponent: 65537 });
  priv = kp.privateKey;
  fs.mkdirSync(path.dirname(privFile), { recursive: true });
  fs.writeFileSync(privFile, priv.export({ type: 'pkcs8', format: 'pem' }));
  console.log('закрытый ключ записан:', privFile);
}
const jwk = crypto.createPublicKey(priv).export({ format: 'jwk' });
const b64u = s => Buffer.from(s, 'base64url');
const n = b64u(jwk.n), e = b64u(jwk.e);
const head = Buffer.alloc(24);
head.writeUInt32LE(0x31415352, 0);          // 'RSA1'
head.writeUInt32LE(n.length * 8, 4);
head.writeUInt32LE(e.length, 8);
head.writeUInt32LE(n.length, 12);           // cbPrime1 = cbPrime2 = 0 (открытый ключ)
const blob = Buffer.concat([head, e, n]);
const rows = [];
for (let i = 0; i < blob.length; i += 16) rows.push('    ' + [...blob.subarray(i, i + 16)].map(b => '0x' + b.toString(16).padStart(2, '0')).join(', '));
fs.writeFileSync(hdrPath, '// Открытый ключ (release), BCRYPT_RSAPUBLIC_BLOB. Сгенерировано tools\\genkey.js.\r\n' +
  'static const unsigned char kReleasePub[] = {\r\n' + rows.join(',\r\n') + '\r\n};\r\n');
console.log('открытый ключ:', hdrPath, blob.length, 'байт');
