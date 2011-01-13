# Copyright (c) 2011 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

'''Unit tests for grit.format.policy_templates.writers.plist_writer'''


import os
import re
import sys
if __name__ == '__main__':
  sys.path.append(os.path.join(os.path.dirname(sys.argv[0]), '../../../..'))

import tempfile
import unittest
import StringIO
from xml.dom import minidom

from grit.format import rc
from grit.format.policy_templates.writers import writer_unittest_common
from grit import grd_reader
from grit import util
from grit.tool import build


class PListWriterUnittest(writer_unittest_common.WriterUnittestCommon):
  '''Unit tests for PListWriter.'''

  def _GetExpectedOutputs(self, product_name, bundle_id, policies):
    '''Substitutes the variable parts into a plist template. The result
    of this function can be used as an expected result to test the output
    of PListWriter.

    Args:
      product_name: The name of the product, normally Chromium or Google Chrome.
      bundle_id: The mac bundle id of the product.
      policies: The list of policies.

    Returns:
      The text of a plist template with the variable parts substituted.
    '''
    return '''
<?xml version="1.0" ?>
<!DOCTYPE plist  PUBLIC '-//Apple//DTD PLIST 1.0//EN'  'http://www.apple.com/DTDs/PropertyList-1.0.dtd'>
<plist version="1">
  <dict>
    <key>pfm_name</key>
    <string>%s</string>
    <key>pfm_description</key>
    <string/>
    <key>pfm_title</key>
    <string/>
    <key>pfm_version</key>
    <string>1</string>
    <key>pfm_domain</key>
    <string>%s</string>
    <key>pfm_subkeys</key>
    %s
  </dict>
</plist>''' % (product_name, bundle_id, policies)

  def testEmpty(self):
    # Test PListWriter in case of empty polices.
    grd = self.PrepareTest('''
      {
        'policy_definitions': [],
        'placeholders': [],
      }''', '''<messages />''')

    output = self.GetOutput(
        grd,
        'fr',
        {'_chromium': '1', 'mac_bundle_id': 'com.example.Test'},
        'plist',
        'en')
    expected_output = self._GetExpectedOutputs(
        'Chromium', 'com.example.Test', '<array/>')
    self.assertEquals(output.strip(), expected_output.strip())

  def testMainPolicy(self):
    # Tests a policy group with a single policy of type 'main'.
    grd = self.PrepareTest('''
      {
        'policy_definitions': [
          {
            'name': 'MainGroup',
            'type': 'group',
            'policies': [{
              'name': 'MainPolicy',
              'type': 'main',
              'supported_on': ['chrome.mac:8-'],
            }],
          },
        ],
        'placeholders': [],
      }''', '''
        <messages>
          <message name="IDS_POLICY_MAINGROUP_CAPTION">This is not tested here.</message>
          <message name="IDS_POLICY_MAINGROUP_DESC">This is not tested here.</message>
          <message name="IDS_POLICY_MAINPOLICY_CAPTION">This is not tested here.</message>
          <message name="IDS_POLICY_MAINPOLICY_DESC">This is not tested here.</message>
        </messages>
      ''')
    output = self.GetOutput(
        grd,
        'fr',
        {'_chromium' : '1', 'mac_bundle_id': 'com.example.Test'},
        'plist',
        'en')
    expected_output = self._GetExpectedOutputs(
        'Chromium', 'com.example.Test', '''<array>
      <dict>
        <key>pfm_name</key>
        <string>MainPolicy</string>
        <key>pfm_description</key>
        <string/>
        <key>pfm_title</key>
        <string/>
        <key>pfm_targets</key>
        <array>
          <string>user-managed</string>
        </array>
        <key>pfm_type</key>
        <string>boolean</string>
      </dict>
    </array>''')
    self.assertEquals(output.strip(), expected_output.strip())

  def testStringPolicy(self):
    # Tests a policy group with a single policy of type 'string'.
    grd = self.PrepareTest('''
      {
        'policy_definitions': [
          {
            'name': 'StringGroup',
            'type': 'group',
            'policies': [{
              'name': 'StringPolicy',
              'type': 'string',
              'supported_on': ['chrome.mac:8-'],
            }],
          },
        ],
        'placeholders': [],
      }''', '''
        <messages>
          <message name="IDS_POLICY_STRINGGROUP_CAPTION">This is not tested here.</message>
          <message name="IDS_POLICY_STRINGGROUP_DESC">This is not tested here.</message>
          <message name="IDS_POLICY_STRINGPOLICY_CAPTION">This is not tested here.</message>
          <message name="IDS_POLICY_STRINGPOLICY_DESC">This is not tested here.</message>
        </messages>
      ''')
    output = self.GetOutput(
        grd,
        'fr',
        {'_chromium' : '1', 'mac_bundle_id': 'com.example.Test'},
        'plist',
        'en')
    expected_output = self._GetExpectedOutputs(
        'Chromium', 'com.example.Test', '''<array>
      <dict>
        <key>pfm_name</key>
        <string>StringPolicy</string>
        <key>pfm_description</key>
        <string/>
        <key>pfm_title</key>
        <string/>
        <key>pfm_targets</key>
        <array>
          <string>user-managed</string>
        </array>
        <key>pfm_type</key>
        <string>string</string>
      </dict>
    </array>''')
    self.assertEquals(output.strip(), expected_output.strip())

  def testIntPolicy(self):
    # Tests a policy group with a single policy of type 'int'.
    grd = self.PrepareTest('''
      {
        'policy_definitions': [
          {
            'name': 'IntGroup',
            'type': 'group',
            'policies': [{
              'name': 'IntPolicy',
              'type': 'int',
              'supported_on': ['chrome.mac:8-'],
            }],
          },
        ],
        'placeholders': [],
      }''', '''
        <messages>
          <message name="IDS_POLICY_INTGROUP_CAPTION">This is not tested here.</message>
          <message name="IDS_POLICY_INTGROUP_DESC">This is not tested here.</message>
          <message name="IDS_POLICY_INTPOLICY_CAPTION">This is not tested here.</message>
          <message name="IDS_POLICY_INTPOLICY_DESC">This is not tested here.</message>
        </messages>
      ''')
    output = self.GetOutput(
        grd,
        'fr',
        {'_chromium' : '1', 'mac_bundle_id': 'com.example.Test'},
        'plist',
        'en')
    expected_output = self._GetExpectedOutputs(
        'Chromium', 'com.example.Test', '''<array>
      <dict>
        <key>pfm_name</key>
        <string>IntPolicy</string>
        <key>pfm_description</key>
        <string/>
        <key>pfm_title</key>
        <string/>
        <key>pfm_targets</key>
        <array>
          <string>user-managed</string>
        </array>
        <key>pfm_type</key>
        <string>integer</string>
      </dict>
    </array>''')
    self.assertEquals(output.strip(), expected_output.strip())

  def testIntEnumPolicy(self):
    # Tests a policy group with a single policy of type 'int-enum'.
    grd = self.PrepareTest('''
      {
        'policy_definitions': [
          {
            'name': 'EnumGroup',
            'type': 'group',
            'policies': [{
              'name': 'EnumPolicy',
              'type': 'int-enum',
              'items': [
                {'name': 'ProxyServerDisabled', 'value': 0},
                {'name': 'ProxyServerAutoDetect', 'value': 1},
              ],
              'supported_on': ['chrome.mac:8-'],
            }],
          },
        ],
        'placeholders': [],
      }''', '''
        <messages>
          <message name="IDS_POLICY_ENUMGROUP_CAPTION">This is not tested here.</message>
          <message name="IDS_POLICY_ENUMGROUP_DESC">This is not tested here.</message>
          <message name="IDS_POLICY_ENUMPOLICY_CAPTION">This is not tested here.</message>
          <message name="IDS_POLICY_ENUMPOLICY_DESC">This is not tested here.</message>
          <message name="IDS_POLICY_ENUM_PROXYSERVERDISABLED_CAPTION">This is not tested here.</message>
          <message name="IDS_POLICY_ENUM_PROXYSERVERAUTODETECT_CAPTION">This is not tested here.</message>
        </messages>
      ''')
    output = self.GetOutput(
        grd,
        'fr',
        {'_google_chrome': '1', 'mac_bundle_id': 'com.example.Test2'},
        'plist',
        'en')
    expected_output = self._GetExpectedOutputs(
        'Google_Chrome', 'com.example.Test2', '''<array>
      <dict>
        <key>pfm_name</key>
        <string>EnumPolicy</string>
        <key>pfm_description</key>
        <string/>
        <key>pfm_title</key>
        <string/>
        <key>pfm_targets</key>
        <array>
          <string>user-managed</string>
        </array>
        <key>pfm_type</key>
        <string>integer</string>
        <key>pfm_range_list</key>
        <array>
          <integer>0</integer>
          <integer>1</integer>
        </array>
      </dict>
    </array>''')
    self.assertEquals(output.strip(), expected_output.strip())

  def testStringEnumPolicy(self):
    # Tests a policy group with a single policy of type 'string-enum'.
    grd = self.PrepareTest('''
      {
        'policy_definitions': [
          {
            'name': 'EnumGroup',
            'type': 'group',
            'policies': [{
              'name': 'EnumPolicy',
              'type': 'string-enum',
              'items': [
                {'name': 'ProxyServerDisabled', 'value': 'one'},
                {'name': 'ProxyServerAutoDetect', 'value': 'two'},
              ],
              'supported_on': ['chrome.mac:8-'],
            }],
          },
        ],
        'placeholders': [],
      }''', '''
        <messages>
          <message name="IDS_POLICY_ENUMGROUP_CAPTION">This is not tested here.</message>
          <message name="IDS_POLICY_ENUMGROUP_DESC">This is not tested here.</message>
          <message name="IDS_POLICY_ENUMPOLICY_CAPTION">This is not tested here.</message>
          <message name="IDS_POLICY_ENUMPOLICY_DESC">This is not tested here.</message>
          <message name="IDS_POLICY_ENUM_PROXYSERVERDISABLED_CAPTION">This is not tested here.</message>
          <message name="IDS_POLICY_ENUM_PROXYSERVERAUTODETECT_CAPTION">This is not tested here.</message>
        </messages>
      ''')
    output = self.GetOutput(
        grd,
        'fr',
        {'_google_chrome': '1', 'mac_bundle_id': 'com.example.Test2'},
        'plist',
        'en')
    expected_output = self._GetExpectedOutputs(
        'Google_Chrome', 'com.example.Test2', '''<array>
      <dict>
        <key>pfm_name</key>
        <string>EnumPolicy</string>
        <key>pfm_description</key>
        <string/>
        <key>pfm_title</key>
        <string/>
        <key>pfm_targets</key>
        <array>
          <string>user-managed</string>
        </array>
        <key>pfm_type</key>
        <string>string</string>
        <key>pfm_range_list</key>
        <array>
          <string>one</string>
          <string>two</string>
        </array>
      </dict>
    </array>''')
    self.assertEquals(output.strip(), expected_output.strip())

  def testNonSupportedPolicy(self):
    # Tests a policy that is not supported on Mac, so it shouldn't
    # be included in the plist file.
    grd = self.PrepareTest('''
      {
        'policy_definitions': [
          {
            'name': 'NonMacGroup',
            'type': 'group',
            'policies': [{
              'name': 'NonMacPolicy',
              'type': 'string',
              'supported_on': ['chrome.linux:8-', 'chrome.win:7-'],
            }],
          },
        ],
        'placeholders': [],
      }''', '''
        <messages>
          <message name="IDS_POLICY_NONMACGROUP_CAPTION">This is not tested here. (1)</message>
          <message name="IDS_POLICY_NONMACGROUP_DESC">This is not tested here. (2)</message>
          <message name="IDS_POLICY_NONMACPOLICY_CAPTION">This is not tested here. (3)</message>
          <message name="IDS_POLICY_NONMACPOLICY_DESC">This is not tested here. (4)</message>
        </messages>
      ''')
    output = self.GetOutput(
        grd,
        'fr',
        {'_google_chrome': '1', 'mac_bundle_id': 'com.example.Test2'},
        'plist',
        'en')
    expected_output = self._GetExpectedOutputs(
        'Google_Chrome', 'com.example.Test2', '''<array/>''')
    self.assertEquals(output.strip(), expected_output.strip())


if __name__ == '__main__':
  unittest.main()
