from websockets.sync.client import connect
import pathlib
import pyaudio
import ssl
import sys

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
    # with connect(f"ws://{bridge_ip}/ws", ping_timeout=5, close_timeout=5) as websocket:
    with connect(f"ws://{bridge_ip}/ws", ping_timeout=None, close_timeout=None, max_size=(1E6, 1E6)) as websocket:
        print("Connected to websocket")

        stream.start_stream()
        while True:
            # Note: recv will wait for either a TEXT or BINARY type packet.
            # The others (hello, close, ping, pong, continue) are handled automatically.
            data = websocket.recv(timeout=None, decode=False)
            print(ham_bridge.ServerInfo.parse(data).to_json(indent=2))
            # stream.write(data)
            
            print(f"Received Message")

try:
    hello()
except KeyboardInterrupt:
    stream.stop_stream()
    p.close()