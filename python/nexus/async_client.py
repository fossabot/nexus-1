import struct
import socket
import asyncio

from .proto import nnquery_pb2 as npb

MAGIC_NUMBER = 0xDEADBEEF
HEADER_SIZE = 12
# Message type
MSG_USER_REGISTER = 1
MSG_USER_REQUEST = 2
MSG_USER_REPLY = 3


class AsyncClient:
    def __init__(self, server_addr, user_id):
        self._server_addr = server_addr
        self._user_id = user_id
        self._req_id = 0
        self._lock = asyncio.Lock()
        self._replies = {}

    async def __aenter__(self):
        host, port = self._server_addr.split(':')
        self._reader, self._writer = await asyncio.open_connection(host, port)
        return self

    async def __aexit__(self, exc_type, exc, tb):
        self._writer.close()

    async def register(self):
        req = npb.RequestProto(user_id=self.user_id)
        msg = self._prepare_message(MSG_USER_REGISTER, req)

        self._writer.write(msg)
        await self._writer.drain()

        reply = await self._wait_reply(req.req_id)
        assert reply.status == 0

    async def request(self, img):
        req = self._prepare_req(img)
        msg = self._prepare_message(MSG_USER_REQUEST, req)

        self._writer.write(msg)
        await self._writer.drain()

        reply = await self._wait_reply(req.req_id)
        return reply

    def _prepare_req(self, img):
        req = npb.RequestProto()
        req.user_id = self._user_id
        req.req_id = self._req_id
        req.input.data_type = npb.DT_IMAGE
        req.input.image.data = img
        req.input.image.format = npb.ImageProto.JPEG
        req.input.image.color = True
        self._req_id += 1
        return req

    def _prepare_message(self, msg_type, request):
        body = request.SerializeToString()
        header = struct.pack('!LLL', MAGIC_NUMBER, msg_type, len(body))
        return header + body

    async def _wait_reply(self, req_id):
        while True:
            async with self._lock:
                reply = self._replies.pop(req_id, None)
                if reply is not None:
                    return reply

                buf = await self._reader.readexactly(HEADER_SIZE)
                magic_no, msg_type, body_length = struct.unpack('!LLL', buf)
                assert magic_no == MAGIC_NUMBER
                assert msg_type == MSG_USER_REPLY

                buf = await self._reader.readexactly(body_length)
                reply = npb.ReplyProto()
                reply.ParseFromString(buf)
                self._replies[reply.req_id] = reply