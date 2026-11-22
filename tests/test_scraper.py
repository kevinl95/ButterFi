"""Unit tests for the Scraper Lambda's pure functions and fetch safety checks.

No network and no AWS: DNS answers, the policy table and HTTP servers are all
local fakes. Run: python -m unittest discover -s tests
"""
import gzip
import http.server
import socket
import struct
import threading
import unittest
from unittest import mock

from lambda_loader import load_lambda

m = load_lambda('ScraperLambda')
PUBLIC_IP = '93.184.215.14'


def decode_payload(payload):
    fmt, data = payload[0], payload[1:]
    if fmt == m.RESP_FORMAT_PLAIN:
        return data
    if fmt == m.RESP_FORMAT_GZIP:
        return gzip.decompress(data)
    if fmt == m.RESP_FORMAT_BROTLI:
        return m.brotli.decompress(data)
    raise AssertionError(f'unknown format byte {fmt}')


def dns_reply(tx_id, answers=(), rcode=0, flags2=0x80, cname_first=False):
    """A DNS reply for 'example.com' A, answers given as IPv4 strings. With
    cname_first, a CNAME (compressed name) precedes the A records."""
    qname = b'\x07example\x03com\x00'
    records = b''
    count = 0
    if cname_first:
        target = b'\x03cdn\xc0\x0c'  # 'cdn.' + pointer back to example.com
        records += b'\xc0\x0c' + struct.pack('!HHIH', 5, 1, 60, len(target)) + target
        count += 1
    for ip in answers:
        records += b'\xc0\x0c' + struct.pack('!HHIH', 1, 1, 60, 4) + socket.inet_aton(ip)
        count += 1
    header = tx_id + bytes([flags2, rcode]) + struct.pack('!HHHH', 1, count, 0, 0)
    return header + qname + b'\x00\x01\x00\x01' + records


class ChunkingTests(unittest.TestCase):
    def test_chunk_text_never_splits_a_character(self):
        text = 'é' * 300 + '漢字' * 100
        chunks = m.chunk_text(text, 7)
        self.assertTrue(all(len(c) <= 7 for c in chunks))
        for c in chunks:
            c.decode('utf-8')  # each chunk decodes on its own
        self.assertEqual(b''.join(chunks).decode('utf-8'), text)

    def test_chunk_bytes(self):
        self.assertEqual(m.chunk_bytes(b'', 4), [b''])
        self.assertEqual(m.chunk_bytes(b'abcdefghij', 4), [b'abcd', b'efgh', b'ij'])

    def test_cut_utf8_backs_up_to_character_start(self):
        raw = 'aé漢'.encode('utf-8')  # 1 + 2 + 3 bytes
        self.assertEqual(m._cut_utf8(raw, 2), b'a')
        self.assertEqual(m._cut_utf8(raw, 3), 'aé'.encode())
        self.assertEqual(m._cut_utf8(raw, 5), 'aé'.encode())
        self.assertEqual(m._cut_utf8(raw, 99), raw)
        self.assertEqual(m._cut_utf8(raw, -1), b'')


class ResponsePayloadTests(unittest.TestCase):
    """build_response_payload: compress, and truncate the source to fit."""

    CHUNK = 236

    def budget(self):
        return self.CHUNK * 255 - 1

    def test_short_text_round_trips(self):
        text = 'Hello, world.'
        payload = m.build_response_payload(text, self.CHUNK)
        self.assertEqual(decode_payload(payload).decode('utf-8'), text)

    def test_compressible_page_is_compressed_whole(self):
        text = '\n'.join(f'# Section {i}\nSome ordinary words about topic {i}.' for i in range(500))
        payload = m.build_response_payload(text, self.CHUNK)
        self.assertNotEqual(payload[0], m.RESP_FORMAT_PLAIN)
        self.assertEqual(decode_payload(payload).decode('utf-8'), text)

    def test_huge_page_fits_255_chunks_as_a_valid_prefix(self):
        import random
        rng = random.Random(7)
        words = ['alpha', 'beta', 'gamma', 'délta', '漢字', 'epsilon', 'zeta']
        text = ' '.join(rng.choice(words) + str(rng.randint(0, 99999)) for _ in range(200000))
        payload = m.build_response_payload(text, self.CHUNK)
        self.assertLessEqual(len(payload) - 1, self.budget())
        self.assertLessEqual(len(m.chunk_bytes(payload, self.CHUNK)), 255)
        out = decode_payload(payload).decode('utf-8')  # strict: no split character
        self.assertTrue(text.startswith(out))
        self.assertGreater(len(out), 10000)

    def test_incompressible_text_falls_back_to_plain_truncation(self):
        import random
        rng = random.Random(3)
        text = ''.join(chr(rng.randint(0x4E00, 0x9FFF)) for _ in range(40000))
        payload = m.build_response_payload(text, self.CHUNK)
        self.assertLessEqual(len(payload) - 1, self.budget())
        self.assertTrue(text.startswith(decode_payload(payload).decode('utf-8')))


class MarkupTests(unittest.TestCase):
    def test_html_to_butterfi_format(self):
        html = ('<html><head><title>T</title><script>var x = 1;</script></head><body>'
                '<h1>Hello</h1><p>Para <a href="/a">link one</a> and '
                '<a href="https://x.org/b">two</a>.</p><ul><li>one</li><li>two</li></ul>'
                '<ol><li>first</li></ol><hr><p>caf&eacute; &#169;</p></body></html>')
        out = m.html_to_butterfi(html, source_url='https://example.com/dir/page')
        lines = out.split('\n')
        self.assertIn('# Hello', lines)
        self.assertIn('Para >1[link one] and >2[two].', lines)
        self.assertIn('- one', lines)
        self.assertIn('1. first', lines)
        self.assertIn('café ©', lines)
        self.assertNotIn('var x', out)
        self.assertEqual(lines[-2:], ['>1 https://example.com/a', '>2 https://x.org/b'])
        self.assertEqual(lines[-3], '---')

    def test_unwrap_ddg_redirect(self):
        wrapped = ('https://duckduckgo.com/l/?uddg=https%3A%2F%2Fwww.weather.gov'
                   '%2Fbou%2F%3Fa%3D1%2526b%3D2&rut=abc')
        self.assertEqual(m.unwrap_ddg_redirect(wrapped), 'https://www.weather.gov/bou/?a=1%26b=2')
        self.assertEqual(m.unwrap_ddg_redirect('https://example.com/l/?uddg=x'),
                         'https://example.com/l/?uddg=x')
        self.assertEqual(m.unwrap_ddg_redirect('https://duckduckgo.com/l/?uddg=javascript:alert(1)'),
                         'https://duckduckgo.com/l/?uddg=javascript:alert(1)')

    def test_query_routing(self):
        with mock.patch.object(m, 'search', return_value='SEARCHED') as search, \
             mock.patch.object(m, 'is_safe_fetch_url', return_value=(False, 'stub')):
            self.assertEqual(m.fetch_and_extract('product rule derivative'), 'SEARCHED')
            self.assertEqual(m.fetch_and_extract('cnn.com'), 'Cannot open that address (stub).')
            self.assertEqual(m.fetch_and_extract('text.npr.org/story'), 'Cannot open that address (stub).')
            self.assertEqual(m.fetch_and_extract('https://x.org'), 'Cannot open that address (stub).')
            self.assertEqual(search.call_count, 1)


class SearchChallengeTests(unittest.TestCase):
    def setUp(self):
        p = mock.patch.object(m, 'is_safe_fetch_url', return_value=(True, 'ok'))
        p.start()
        self.addCleanup(p.stop)

    def test_long_query_retries_short(self):
        replies = [(202, 'u', '<div class="anomaly-modal">duck</div>'),
                   (200, 'u', '<p>result</p>')]
        with mock.patch.object(m, '_fetch', side_effect=replies) as fetch:
            out = m.search('one two three four five six seven eight')
        self.assertTrue(out.startswith('(Search shortened to: one two three four five)'))
        self.assertIn('result', out)
        self.assertIn('one%20two%20three%20four%20five', fetch.call_args_list[1].args[0])

    def test_short_query_challenged_says_so(self):
        with mock.patch.object(m, '_fetch', return_value=(202, 'u', 'x')) as fetch:
            self.assertEqual(m.search('boulder weather'), m.SEARCH_CHALLENGED_MSG)
        self.assertEqual(fetch.call_count, 1)

    def test_challenged_twice(self):
        with mock.patch.object(m, '_fetch', return_value=(200, 'u', 'anomaly-modal')):
            self.assertEqual(m.search('a b c d e f g'), m.SEARCH_CHALLENGED_MSG)


class PolicyTests(unittest.TestCase):
    def setUp(self):
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(m, 'POLICY_TABLE', 'policy').start()
        m._policy_cache.update(ts=0.0, allow=None, block=None)

    def fake_lists(self, allow, block):
        return mock.patch.object(m, '_query_policy_list',
                                 side_effect=lambda t: allow if t == 'ALLOW' else block)

    def test_host_on_list_matches_subdomains_only(self):
        self.assertTrue(m._host_on_list('news.example.com', ['example.com']))
        self.assertTrue(m._host_on_list('EXAMPLE.com.', ['example.com']))
        self.assertFalse(m._host_on_list('badexample.com', ['example.com']))
        self.assertFalse(m._host_on_list('example.com', ['']))

    def test_block_wins_and_allow_only_mode(self):
        with self.fake_lists(['school.edu', 'bad.school.edu'], ['bad.school.edu']).start():
            self.assertTrue(m.is_url_permitted('https://www.school.edu/x')[0])
            self.assertFalse(m.is_url_permitted('https://bad.school.edu/')[0])
            self.assertFalse(m.is_url_permitted('https://other.com/')[0])

    def test_cold_start_read_error_fails_closed(self):
        with mock.patch.object(m, '_query_policy_list', side_effect=RuntimeError('ddb down')):
            ok, msg = m.is_url_permitted('https://example.com/')
        self.assertFalse(ok)
        self.assertEqual(msg, m.POLICY_UNAVAILABLE_MSG)

    def test_warm_refresh_error_keeps_last_known_good(self):
        with self.fake_lists(['school.edu'], []):
            self.assertTrue(m.is_url_permitted('https://school.edu/')[0])
        m._policy_cache['ts'] = 0.0  # force a refresh
        with mock.patch.object(m, '_query_policy_list', side_effect=RuntimeError('ddb down')):
            self.assertTrue(m.is_url_permitted('https://school.edu/')[0])
            self.assertFalse(m.is_url_permitted('https://other.com/')[0])

    def test_no_policy_table_is_open_web(self):
        with mock.patch.object(m, 'POLICY_TABLE', ''):
            self.assertTrue(m.is_url_permitted('https://anything.example/')[0])


class DnsParserTests(unittest.TestCase):
    TX = b'\x12\x34'

    def test_a_records(self):
        self.assertEqual(m.parse_dns_a_reply(dns_reply(self.TX, [PUBLIC_IP, '1.2.3.4']), self.TX),
                         [PUBLIC_IP, '1.2.3.4'])

    def test_cname_then_a(self):
        reply = dns_reply(self.TX, [PUBLIC_IP], cname_first=True)
        self.assertEqual(m.parse_dns_a_reply(reply, self.TX), [PUBLIC_IP])

    def test_nxdomain_is_empty(self):
        self.assertEqual(m.parse_dns_a_reply(dns_reply(self.TX, rcode=3), self.TX), [])

    def test_bad_replies_raise(self):
        for reply in (dns_reply(b'\x00\x00', [PUBLIC_IP]),        # wrong transaction ID
                      dns_reply(self.TX, rcode=2),                # SERVFAIL
                      dns_reply(self.TX, [PUBLIC_IP], flags2=0x82),  # truncated
                      b'\x12'):
            with self.assertRaises(ValueError):
                m.parse_dns_a_reply(reply, self.TX)


class ResolvePublicTests(unittest.TestCase):
    def dns(self, answers):
        """Patch the Cloudflare query: answers is a list or an exception."""
        def fake(name, server, timeout=3):
            if isinstance(answers, Exception):
                raise answers
            return list(answers)
        return mock.patch.object(m, '_dns_query_a', side_effect=fake)

    def test_public_answer(self):
        with self.dns([PUBLIC_IP]) as q:
            self.assertEqual(m.resolve_public('Example.COM.'), [PUBLIC_IP])
        self.assertEqual(q.call_args.args[:2], ('example.com', '1.1.1.3'))

    def test_filter_unreachable_fails_closed(self):
        with self.dns(socket.timeout('timed out')) as q:
            with self.assertRaises(m.FetchBlocked) as cm:
                m.resolve_public('example.com')
        self.assertIn('safety filter could not be reached', str(cm.exception))
        self.assertEqual([c.args[1] for c in q.call_args_list], ['1.1.1.3', '1.0.0.3'])

    def test_second_resolver_used_when_first_fails(self):
        calls = []

        def fake(name, server, timeout=3):
            calls.append(server)
            if server == '1.1.1.3':
                raise socket.timeout()
            return [PUBLIC_IP]
        with mock.patch.object(m, '_dns_query_a', side_effect=fake):
            self.assertEqual(m.resolve_public('example.com'), [PUBLIC_IP])
        self.assertEqual(calls, ['1.1.1.3', '1.0.0.3'])

    def test_family_block_and_nxdomain(self):
        with self.dns(['0.0.0.0']):
            with self.assertRaisesRegex(m.FetchBlocked, 'blocked by the safety filter'):
                m.resolve_public('adult.example')
        with self.dns([]):
            with self.assertRaisesRegex(m.FetchBlocked, 'could not be found'):
                m.resolve_public('nope.example')

    def test_private_answers_refused(self):
        for ip in ('127.0.0.1', '10.0.0.5', '169.254.169.254', '192.168.1.1', '100.64.0.1', '224.0.0.1'):
            with self.dns([PUBLIC_IP, ip]):
                with self.assertRaisesRegex(m.FetchBlocked, 'non-public'):
                    m.resolve_public('rebind.example')

    def test_ip_literals_refused_without_dns(self):
        with self.dns([PUBLIC_IP]) as q:
            for host in ('8.8.8.8', '127.0.0.1', '::1', '2606:4700::1111'):
                with self.assertRaisesRegex(m.FetchBlocked, 'typed as numbers'):
                    m.resolve_public(host)
        q.assert_not_called()

    def test_amazonaws_hosts_are_filtered_like_any_other(self):
        with self.dns(['0.0.0.0']) as q:
            with self.assertRaises(m.FetchBlocked):
                m.resolve_public('evil-site.s3-website-us-east-1.amazonaws.com')
        q.assert_called_once()

    def test_invalid_names(self):
        with self.dns([PUBLIC_IP]):
            for host in ('', 'a..b', 'x' * 64 + '.com'):
                with self.assertRaises(m.FetchBlocked):
                    m.resolve_public(host)

    def test_idna_name_is_encoded(self):
        with self.dns([PUBLIC_IP]) as q:
            m.resolve_public('bücher.example')
        self.assertEqual(q.call_args.args[0], 'xn--bcher-kva.example')

    def test_is_safe_fetch_url(self):
        with self.dns([PUBLIC_IP]):
            self.assertEqual(m.is_safe_fetch_url('https://example.com/x'), (True, 'ok'))
            self.assertFalse(m.is_safe_fetch_url('ftp://example.com/')[0])
            self.assertFalse(m.is_safe_fetch_url('file:///etc/passwd')[0])
            self.assertFalse(m.is_safe_fetch_url('http://169.254.169.254/latest/')[0])
            self.assertFalse(m.is_safe_fetch_url('http://example.com:99999/')[0])


class _Handler(http.server.BaseHTTPRequestHandler):
    routes = {}

    def do_GET(self):
        status, headers, body = self.routes[self.path]
        self.send_response(status)
        for k, v in headers.items():
            self.send_header(k, v)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


class FetchTests(unittest.TestCase):
    """_fetch against a local server. 'site.test' resolves (through the real
    resolve_public, with a fake Cloudflare answer) to a public-looking IP, and
    the TCP connect to that IP is redirected to the local server, so the
    pinning path is the one under test."""

    @classmethod
    def setUpClass(cls):
        cls.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), _Handler)
        cls.port = cls.server.server_address[1]
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()

    def setUp(self):
        self.answers = {'site.test': [PUBLIC_IP]}
        self.connects = []
        real_connect = socket.create_connection

        def fake_dns(name, server, timeout=3):
            return list(self.answers.get(name, []))

        def fake_connect(address, *args, **kwargs):
            self.connects.append(address[0])
            if address[0] == PUBLIC_IP:
                return real_connect(('127.0.0.1', address[1]), *args, **kwargs)
            raise AssertionError(f'connected to unvetted address {address}')

        for p in (mock.patch.object(m, '_dns_query_a', side_effect=fake_dns),
                  mock.patch.object(m.socket, 'create_connection', side_effect=fake_connect),
                  mock.patch.object(m, 'is_url_permitted', return_value=(True, 'ok'))):
            p.start()
        self.addCleanup(mock.patch.stopall)
        _Handler.routes = {}

    def url(self, path, host='site.test'):
        return f'http://{host}:{self.port}{path}'

    def test_html_page(self):
        _Handler.routes['/ok'] = (200, {'Content-Type': 'text/html; charset=utf-8'}, b'<p>hi</p>')
        status, final_url, html = m._fetch(self.url('/ok'), timeout=5)
        self.assertEqual((status, html), (200, '<p>hi</p>'))
        self.assertEqual(self.connects, [PUBLIC_IP])

    def test_pdf_refused_before_reading(self):
        _Handler.routes['/doc'] = (200, {'Content-Type': 'application/pdf'}, b'%PDF-1.7' + b'x' * 1000)
        with self.assertRaisesRegex(m.FetchBlocked, 'a PDF file'):
            m._fetch(self.url('/doc'), timeout=5)

    def test_binary_refused(self):
        _Handler.routes['/z'] = (200, {'Content-Type': 'application/zip'}, b'PK..')
        with self.assertRaisesRegex(m.FetchBlocked, r'a file \(application/zip\)'):
            m._fetch(self.url('/z'), timeout=5)

    def test_large_body_is_capped(self):
        big = b'<p>' + b'a' * (m.FETCH_MAX_BYTES + 500000) + b'</p>'
        _Handler.routes['/big'] = (200, {'Content-Type': 'text/html'}, big)
        _, _, html = m._fetch(self.url('/big'), timeout=10)
        self.assertEqual(len(html), m.FETCH_MAX_BYTES)

    def test_dns_rebinding_cannot_reach_private_address(self):
        """The pre-check sees a public answer; by connect time the name points
        at loopback. The connection re-resolves and refuses it."""
        _Handler.routes['/ok'] = (200, {'Content-Type': 'text/html'}, b'ok')
        answers = iter([[PUBLIC_IP], ['127.0.0.1']])
        with mock.patch.object(m, '_dns_query_a', side_effect=lambda *a, **k: next(answers)):
            self.assertTrue(m.is_safe_fetch_url(self.url('/ok', 'rebind.test'))[0])
            with self.assertRaisesRegex(m.FetchBlocked, 'non-public'):
                m._fetch(self.url('/ok', 'rebind.test'), timeout=5)
        self.assertEqual(self.connects, [])

    def test_redirect_to_internal_address_blocked(self):
        _Handler.routes['/r'] = (302, {'Location': 'http://169.254.169.254/latest/meta-data/'}, b'')
        with self.assertRaisesRegex(Exception, 'blocked redirect'):
            m._fetch(self.url('/r'), timeout=5)
        self.assertEqual(self.connects, [PUBLIC_IP])

    def test_fetch_and_extract_reports_refusal(self):
        _Handler.routes['/doc'] = (200, {'Content-Type': 'application/pdf'}, b'%PDF')
        out = m.fetch_and_extract(self.url('/doc'))
        self.assertTrue(out.startswith('Cannot open that address (it is a PDF file'), out)


if __name__ == '__main__':
    unittest.main()
