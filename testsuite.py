#!/usr/bin/python
# TODO: Use tracy (https://github.com/MerlijnWajer/tracy) to see if lwan
#       performs certain system calls. This should speed up the mmap tests
#       considerably and make it possible to perform more low-level tests.

import subprocess
import time
import unittest
import requests


class LwanTest(unittest.TestCase):
  def setUp(self):
    self.lwan = subprocess.Popen(['./build/lwan'],
          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    while True:
      try:
        requests.get('http://127.0.0.1:8080/hello')
        return self.lwan
      except requests.ConnectionError:
        pass

  def tearDown(self):
    self.lwan.kill()


class TestBasicRequest(LwanTest):
  def test_head_request_file(self):
    r = requests.head('http://127.0.0.1:8080/100.html',
          headers={'Accept-Encoding': 'foobar'})

    self.assertEqual(r.status_code, 200)

    self.assertTrue('content-type' in r.headers)
    self.assertEqual(r.headers['content-type'], 'text/html')

    self.assertTrue('content-length' in r.headers)
    self.assertEqual(r.headers['content-length'], '100')

    self.assertEqual(r.text, '')


  def test_head_request_hello(self):
    r = requests.head('http://127.0.0.1:8080/hello',
          headers={'Accept-Encoding': 'foobar'})

    self.assertEqual(r.status_code, 200)

    self.assertTrue('content-type' in r.headers)
    self.assertEqual(r.headers['content-type'], 'text/plain')

    self.assertTrue('content-length' in r.headers)
    self.assertEqual(int(r.headers['content-length']), len('Hello, world!'))

    self.assertEqual(r.text, '')


  def test_uncompressed_small_file(self):
    r = requests.get('http://127.0.0.1:8080/100.html',
          headers={'Accept-Encoding': 'foobar'})

    self.assertEqual(r.status_code, 200)

    self.assertTrue('content-type' in r.headers)
    self.assertEqual(r.headers['content-type'], 'text/html')

    self.assertTrue('content-length' in r.headers)
    self.assertEqual(r.headers['content-length'], '100')

    self.assertEqual(r.text, 'X' * 100)


  def test_compressed_small_file(self):
    r = requests.get('http://127.0.0.1:8080/100.html',
          headers={'Accept-Encoding':'deflate'})

    self.assertEqual(r.status_code, 200)

    self.assertTrue('content-type' in r.headers)
    self.assertEqual(r.headers['content-type'], 'text/html')

    self.assertTrue('content-length' in r.headers)
    self.assertLess(int(r.headers['content-length']), 100)

    self.assertTrue('content-encoding' in r.headers)
    self.assertEqual(r.headers['content-encoding'], 'deflate')

    self.assertEqual(r.text, 'X' * 100)


  def test_has_lwan_server_header(self):
    r = requests.get('http://127.0.0.1:8080/100.html')
    self.assertTrue('server' in r.headers)
    self.assertEqual(r.headers['server'], 'lwan')


def TestHelloWorld(LwanTest):
  def test_has_custom_header(self):
    r = requests.get('http://127.0.0.1:8080/hello')

    self.assertTrue('x-the-answer-to-the-universal-question' in r.headers)
    self.assertEqual(r.headers['x-the-answer-to-the-universal-question'], 42)


  def test_no_param(self):
    r = requests.get('http://127.0.0.1:8080/hello')

    self.assertEqual(r.status_code, 200)

    self.assertTrue('content-type' in r.headers)
    self.assertEqual(r.headers['content-type'], 'text/plain')

    self.assertTrue('content-length' in r.headers)
    self.assertEqual(int(r.headers['content-length']), len('Hello, world!'))

    self.assertEqual(r.text, 'Hello, world!')


  def test_with_param(self):
    r = requests.get('http://127.0.0.1:8080/hello?name=testsuite')

    self.assertEqual(r.status_code, 200)

    self.assertTrue('content-type' in r.headers)
    self.assertEqual(r.headers['content-type'], 'text/plain')

    self.assertTrue('content-length' in r.headers)
    self.assertEqual(int(r.headers['content-length']),
          len('Hello, testsuite!'))

    self.assertEqual(r.text, 'Hello, testsuite!')


class TestCache(LwanTest):
  def mmaps(self, f):
    f = f + '\n'
    return (l.endswith(f) for l in
                file('/proc/%d/maps' % self.lwan.pid))


  def count_mmaps(self, f):
    return sum(self.mmaps(f))


  def is_mmapped(self, f):
    return any(self.mmaps(f))


  def test_cache_munmaps_conn_close(self):
    r = requests.get('http://127.0.0.1:8080/100.html')

    self.assertTrue(self.is_mmapped('files_root/100.html'))
    time.sleep(20)
    self.assertFalse(self.is_mmapped('files_root/100.html'))


  def test_cache_munmaps_conn_keep_alive(self):
    s = requests.Session()
    r = s.get('http://127.0.0.1:8080/100.html')

    self.assertTrue(self.is_mmapped('files_root/100.html'))
    time.sleep(20)
    self.assertFalse(self.is_mmapped('files_root/100.html'))


  def test_cache_does_not_mmap_large_files(self):
    r = requests.get('http://127.0.0.1:8080/zero')
    self.assertFalse(self.is_mmapped('files_root/zero'))


  def test_cache_mmaps_once_conn_keep_alive(self):
    s = requests.Session()

    for request in range(5):
      r = s.get('http://127.0.0.1:8080/100.html')
      self.assertEqual(self.count_mmaps('files_root/100.html'), 1)


  def test_cache_mmaps_once_conn_close(self):
    for request in range(5):
      requests.get('http://127.0.0.1:8080/100.html')
      self.assertEqual(self.count_mmaps('files_root/100.html'), 1)


  def test_cache_mmaps_once_even_after_timeout(self):
    for request in range(5):
      requests.get('http://127.0.0.1:8080/100.html')
      self.assertEqual(self.count_mmaps('files_root/100.html'), 1)

    time.sleep(10)

    requests.get('http://127.0.0.1:8080/100.html')
    self.assertEqual(self.count_mmaps('files_root/100.html'), 1)

if __name__ == '__main__':
  unittest.main()
