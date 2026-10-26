import assert from 'node:assert/strict';
import test from 'node:test';
import { echEvidence } from '../check-ech.mjs';
const url = 'https://example.com/';
const response = (host, accepted) => ({ url: `https://${host}/`, status: 200,
  protocol: 'h2', securityDetails: { encryptedClientHello: accepted, protocol: 'TLS 1.3' } });
test('only actual ECH acceptance on the target host counts', () => {
  assert.equal(echEvidence(url, [response('example.com', true)]).result, 'accepted');
  assert.equal(echEvidence(url, [response('example.com', false)]).result, 'not-accepted');
  assert.equal(echEvidence(url, [response('example.com', undefined)]).result, 'inconclusive');
  assert.equal(echEvidence(url, [response('elsewhere.com', true)]).result, 'inconclusive');
  assert.equal(echEvidence(url, []).result, 'inconclusive');
});
test('a later non-ECH target response cannot inherit an earlier accepted handshake', () => {
  assert.equal(echEvidence(url, [response('example.com', true), response('example.com', false)]).result, 'not-accepted');
});
