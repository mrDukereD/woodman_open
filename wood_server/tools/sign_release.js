// Подпись архива обновления: <архив>.sig = base64(RSA-SHA256 PKCS#1 v1.5 подпись байтов архива).
//   node sign_release.js "archive.zip" [путь\к\release_private.pem]
// Ключ по умолчанию - <корень репозитория>\keys\release_private.pem (создаётся: node genkey.js; в git не попадает).
// Проверяет wood_server.exe update (pubkey_release.h). Вызывать ПОСЛЕ создания архива и выкладывать .sig рядом с ним.
'use strict';
const fs = require('fs'), path = require('path'), crypto = require('crypto');
const zip = process.argv[2];
const keyFile = process.argv[3] || path.resolve(__dirname, '..', '..', 'keys', 'release_private.pem');
if (!zip || !fs.existsSync(zip)) { console.log('node sign_release.js <архив.zip> [release_private.pem]'); process.exit(2); }
if (!fs.existsSync(keyFile)) { console.log('нет закрытого ключа: ' + keyFile + ' (node genkey.js)'); process.exit(2); }
const sig = crypto.sign('sha256', fs.readFileSync(zip), crypto.createPrivateKey(fs.readFileSync(keyFile)));
fs.writeFileSync(zip + '.sig', sig.toString('base64') + '\n');
console.log('подпись записана: ' + zip + '.sig (' + sig.length + ' байт)');
