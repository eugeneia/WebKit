# Copyright (C) Research In Motion Limited 2011. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
# following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
# disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
# following disclaimer in the documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS “AS IS” AND ANY EXPRESS OR
# IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
# APPLE INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
# HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
# TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
# SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


import time
from pywebsocket3 import stream
from pywebsocket3.handshake.hybi import compute_accept_from_unicode


def web_socket_do_extra_handshake(request):
    # This simulates a broken server that sends a WebSocket frame before the
    # handshake, and more frames afterwards.  It is important that if this
    # happens the client does not buffer all the frames as the server continues
    # to send more data - it should abort after reading a reasonable number of
    # bytes (set arbitrarily to 1024).

    msg = b'HTTP/1.1 101 Switching \0Protocols\r\n'
    msg += b'Upgrade: websocket\r\n'
    msg += b'Connection: Upgrade\r\n'
    msg += b'Sec-WebSocket-Accept: '
    msg += compute_accept_from_unicode(request.headers_in['Sec-WebSocket-Key'])
    msg += b'\r\n\r\n'
    request.connection.write(msg)
    # continue writing data until the client disconnects
    while True:
        time.sleep(1)
        numFrames = 1024 // len(msg)  # write over 1024 bytes including the above handshake
        for i in range(0, numFrames):
            request.connection.write(msg)


def web_socket_transfer_data(request):
    pass
