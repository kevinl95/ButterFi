"""Unit tests for the Downlink (resend) Lambda, with fake DynamoDB/iotwireless."""
import base64
import unittest
from unittest import mock

from lambda_loader import load_lambda

m = load_lambda('DownlinkLambda')
DEV = 'dev-1'


class ClientError(Exception):
    def __init__(self, code):
        super().__init__(code)
        self.response = {'Error': {'Code': code}}


class FakeTable:
    def __init__(self, request_id, total, counter=0):
        self.counter = counter
        self.items = [{'deviceId': DEV, 'chunkKey': f'{request_id:03d}#{i:04d}', 'total': total,
                       'data': base64.b64encode(bytes([i])).decode()} for i in range(total)]

    def query(self, **kwargs):
        prefix = kwargs['ExpressionAttributeValues'][':prefix']
        return {'Items': [it for it in self.items if it['chunkKey'].startswith(prefix)]}

    def update_item(self, **kwargs):
        self.counter += kwargs['ExpressionAttributeValues'][':n']
        return {'Attributes': {'seqCounter': self.counter}}


class FakeWireless:
    def __init__(self, failures=()):
        self.sent = []
        self.failures = list(failures)  # error codes to raise, in order, before succeeding

    def send_data_to_wireless_device(self, **kwargs):
        if self.failures:
            raise ClientError(self.failures.pop(0))
        self.sent.append(kwargs)
        return {'MessageId': str(len(self.sent))}


class DownlinkTests(unittest.TestCase):
    def run_handler(self, table, wireless, start_idx=0, request_id=5):
        db = mock.Mock()
        db.Table.return_value = table
        with mock.patch.object(m, 'dynamodb', db), \
             mock.patch.object(m, 'iot_wireless', wireless), \
             mock.patch.object(m.time, 'sleep'):
            return m.handler({'deviceId': DEV, 'requestId': request_id, 'startIdx': start_idx}, None)

    def sent_indexes(self, wireless):
        return [base64.b64decode(s['PayloadData'])[2] for s in wireless.sent]

    def test_sends_a_full_batch_from_start_index(self):
        table, wl = FakeTable(5, 40), FakeWireless()
        out = self.run_handler(table, wl, start_idx=7)
        self.assertEqual(out['sent'], m.RESEND_BATCH)
        self.assertEqual(self.sent_indexes(wl), list(range(7, 7 + m.RESEND_BATCH)))
        header = base64.b64decode(wl.sent[0]['PayloadData'])[:4]
        self.assertEqual(list(header), [0x81, 5, 7, 40])

    def test_last_partial_batch(self):
        table, wl = FakeTable(5, 12), FakeWireless()
        self.assertEqual(self.run_handler(table, wl, start_idx=9)['sent'], 3)

    def test_every_downlink_gets_a_fresh_seq(self):
        table, wl = FakeTable(5, 40, counter=16382), FakeWireless()
        self.run_handler(table, wl)
        self.run_handler(table, wl, start_idx=m.RESEND_BATCH)
        seqs = [s['WirelessMetadata']['Sidewalk']['Seq'] for s in wl.sent]
        expected = [(16382 + i) % 16384 for i in range(2 * m.RESEND_BATCH)]  # wraps at 16384
        self.assertEqual(seqs, expected)
        self.assertEqual(len(set(seqs)), len(seqs))

    def test_throttle_is_retried(self):
        table, wl = FakeTable(5, 2), FakeWireless(failures=['ThrottlingException'] * 2)
        self.assertEqual(self.run_handler(table, wl)['sent'], 2)

    def test_other_errors_end_the_batch(self):
        table, wl = FakeTable(5, 4), FakeWireless(failures=['ValidationException'])
        self.assertEqual(self.run_handler(table, wl)['sent'], 0)
        self.assertEqual(wl.sent, [])

    def test_persistent_throttle_gives_up(self):
        table, wl = FakeTable(5, 4), FakeWireless(failures=['ThrottlingException'] * 10)
        self.assertEqual(self.run_handler(table, wl)['sent'], 0)

    def test_retry_budget_is_bounded(self):
        clock = iter(range(0, 1000, 3))  # every monotonic() read advances 3 s
        table, wl = FakeTable(5, 4), FakeWireless(failures=['ThrottlingException'] * 3)
        with mock.patch.object(m.time, 'monotonic', side_effect=lambda: next(clock)):
            self.assertEqual(self.run_handler(table, wl)['sent'], 0)

    def test_missing_request(self):
        self.assertEqual(self.run_handler(FakeTable(5, 4), FakeWireless(), request_id=9)['statusCode'], 404)


if __name__ == '__main__':
    unittest.main()
