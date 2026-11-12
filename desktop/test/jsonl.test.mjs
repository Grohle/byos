import test from 'node:test';
import assert from 'node:assert/strict';
import { LineSplitter, parseEventLine } from '../lib/jsonl.mjs';

test('lines split across chunks, CRLF and a multi-byte character cut in half', () => {
  const lines = [];
  const s = new LineSplitter((l) => lines.push(l));
  const text = Buffer.from('{"a":"Ñandú"}\r\n\n{"b":2}\n{"c":3}', 'utf8');
  // Cut inside the two-byte "Ñ" (offset 7) and inside the CRLF.
  s.push(text.subarray(0, 7));
  s.push(text.subarray(7, 15));
  s.push(text.subarray(15, 17));
  s.push(text.subarray(17));
  s.end();
  assert.deepEqual(lines, ['{"a":"Ñandú"}', '{"b":2}', '{"c":3}']);
});

test('valid events are accepted', () => {
  const r = parseEventLine('{"v":1,"event":"result","t":5,"ok":true,"count":2}');
  assert.equal(r.ok, true);
  assert.equal(r.event.count, 2);
  assert.equal(parseEventLine('{"v":1,"event":"error","t":5,"ok":false,"code":"remote"}').ok, true);
});

test('bad lines are rejected with a reason', () => {
  for (const [line, why] of [
    ['not json', 'not JSON'],
    ['[1]', 'not an object'],
    ['{"v":2,"event":"start"}', 'unknown output version 2'],
    ['{"v":1,"event":"hello"}', 'unknown event "hello"'],
    ['{"v":1,"event":"result","ok":false}', 'result without ok:true'],
    ['{"v":1,"event":"error","ok":false}', 'error without ok:false and code'],
  ]) {
    const r = parseEventLine(line);
    assert.equal(r.ok, false, line);
    assert.equal(r.reason, why);
  }
});
