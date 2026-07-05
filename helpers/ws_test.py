import pathlib
import ssl

from websockets.sync.client import connect

bridge_ip = "10.42.0.207"

# ssl_context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
# localhost_pem = pathlib.Path(__file__).with_name("ham-bridge.pem")
# ssl_context.load_verify_locations(localhost_pem)
# ssl_context.check_hostname = False;

def hello():
    # with connect(f"wss://{bridge_ip}/ws", ssl=ssl_context, ping_timeout=5, close_timeout=5) as websocket:
    with connect(f"ws://{bridge_ip}/ws", ping_timeout=5, close_timeout=5) as websocket:
        websocket.send("Hello world!")
        print("Sent Hello World")

        while True:
            message = websocket.recv()
            print(f"Received {message}")

hello()