import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import path from 'node:path';
import { REPO_DIR } from '../lib/cli.mjs';
import { checkPath, joinPath, parentPath } from '../lib/btp-path.mjs';

const vectors = JSON.parse(readFileSync(path.join(REPO_DIR, 'tests', 'btp', 'vectors.json'), 'utf8'));
const hex = (h) => Uint8Array.from(Buffer.from(h, 'hex'));

test('every valid path in tests/btp/vectors.json is accepted', () => {
  assert.ok(vectors.paths.valid.length > 0);
  for (const v of vectors.paths.valid) {
    assert.equal(checkPath(hex(v.path_hex)), null, v.path);
    assert.equal(checkPath(v.path), null, v.path);
  }
});

test('every invalid path in tests/btp/vectors.json gives the same reason', () => {
  assert.ok(vectors.paths.invalid.length > 0);
  for (const v of vectors.paths.invalid) {
    assert.equal(checkPath(hex(v.path_hex)), v.reason, v.path_hex.slice(0, 40));
  }
});

test('empty path is only allowed as the root', () => {
  assert.equal(checkPath(''), 'empty');
  assert.equal(checkPath('', { allowEmpty: true }), null);
});

test('lone surrogates count as bad UTF-8', () => {
  assert.equal(checkPath('a\uD800b'), 'bad_utf8');
});

test('join and parent', () => {
  assert.equal(joinPath('', 'Music'), 'Music');
  assert.equal(joinPath('Music', 'ABBA'), 'Music/ABBA');
  assert.equal(parentPath('Music/ABBA'), 'Music');
  assert.equal(parentPath('Music'), '');
});
