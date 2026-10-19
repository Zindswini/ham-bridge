from websockets.sync.client import connect
import asyncio
import pyaudio
import ssl
import sys
import time

sys.path.insert(0,"../components/ham-bridge-protos/compiled/python")
from bridge_protos.Zindswini import ham_bridge

bridge_ip = "10.42.0.207"

# ssl_context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
# localhost_pem = pathlib.Path(__file__).with_name("ham-bridge.pem")
# ssl_context.load_verify_locations(localhost_pem)
# ssl_context.check_hostname = False;

p = pyaudio.PyAudio()
stream = p.open(rate=44100, channels=2, format=pyaudio.paInt16, output=True, frames_per_buffer=256)

def hello():
    # with connect(f"wss://{bridge_ip}/ws", ssl=ssl_context, ping_timeout=5, close_timeout=5) as websocket:
    with connect(f"ws://{bridge_ip}/ws", ping_interval=5, ping_timeout=2, close_timeout=None, max_size=(int(1E6), int(1E6))) as websocket:
        print("Connected to websocket")

        stream.start_stream()

        # Send a hello request
        request = ham_bridge.Envelope(
            request=ham_bridge.Request(
                request_id=999,
                hello_request=ham_bridge.HelloRequest(
                    protocol_version=1, client_nickname="ws_test"
                )
            )).SerializeToString().__bytes__()
        
        websocket.send(message=bytes(request), text=False)

        # Expect and decode hello response:
        data = websocket.recv(timeout=None, decode=None)
        if type(data) is bytes:
            print("Got bytes, decoding as proto")
            print(ham_bridge.Envelope.parse(data).to_json(indent=2))

        # Ping in loop
        while True:
            # Note: recv will wait for either a TEXT or BINARY type packet.
            # The others (hello, close, ping, pong, continue) are handled automatically.
            request = ham_bridge.Envelope(request=ham_bridge.Request(
                request_id=999,
                ping_request=ham_bridge.PingRequest(client_time_us=(time.time_ns() // 1000))
            )).SerializeToString().__bytes__()
            websocket.send(message=bytes(request), text=False)
            print("Sent ping request")
            data = websocket.recv(timeout=None, decode=None)
            if type(data) is bytes:
                print(ham_bridge.Envelope.parse(data).to_json(indent=2))
            elif type(data) is str:
                print(f"Got text: {data}")
            
            # stream.write(data)
            
            time.sleep(0.5)

try:
    hello()
except KeyboardInterrupt:
    stream.stop_stream()
    p.close(stream)