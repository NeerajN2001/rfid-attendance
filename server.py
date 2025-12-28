import asyncio
import websockets
import json
# Removed: import logging

# Dictionary to store connected clients, mapping client name to its WebSocket object
CLIENTS = {}
SERVER_PORT = 8765

async def router(websocket, path=None):
    """
    Handles incoming WebSocket connections and message routing.
    This function now uses print() statements for output instead of the logging module.
    """
    client_name = None
    try:
        # 1. Registration Phase: Wait for the first message which should contain the client's name
        registration_message = await websocket.recv()
        data = json.loads(registration_message)

        # Check for registration payload structure used by client files: {"type": "register", "name": "..."}
        if data.get("type") == "register" and "name" in data:
            client_name = data["name"]
            
            # --- Inline Registration Logic ---
            CLIENTS[client_name] = websocket
            print(f"[SERVER] Client '{client_name}' connected. Total clients: {len(CLIENTS)}")
            await websocket.send(json.dumps({"status": "connected", "message": f"Welcome, {client_name}! Registered successfully."}))
            # ---------------------------------
            
        else:
            print("[SERVER] First message was not a valid registration. Closing connection.")
            await websocket.close(code=1008, reason="Registration failed")
            return

        # 2. Main Message Loop
        async for message in websocket:
            try:
                data = json.loads(message)
                
                # Expected format from clients: { "to": "recipient_name", "msg": { ... } }
                recipient_name = data.get("to")
                payload = data.get("msg")
                
                if recipient_name and payload:
                    recipient_ws = CLIENTS.get(recipient_name)
                    
                    if recipient_ws:
                        # Add sender info to the payload before forwarding
                        payload_with_sender = {
                            "from": client_name,
                            "msg": payload
                        }
                        
                        await recipient_ws.send(json.dumps(payload_with_sender))
                        print(f"[SERVER] {client_name} → {recipient_name}. Payload: {payload}")
                    else:
                        error_msg = {"error": f"Recipient '{recipient_name}' not found."}
                        await websocket.send(json.dumps(error_msg))
                        print(f"[SERVER] Failed to route from '{client_name}': Recipient '{recipient_name}' not found.")
                else:
                    error_msg = {"error": "Invalid message format. Missing 'to' or 'msg'."}
                    await websocket.send(json.dumps(error_msg))
                    print(f"[SERVER] Received invalid message from '{client_name}': {message}")

            except json.JSONDecodeError:
                error_msg = {"error": "Invalid JSON received."}
                await websocket.send(json.dumps(error_msg))
                print(f"[SERVER] Invalid JSON received from '{client_name}'.")
            except Exception as e:
                print(f"[SERVER] Error handling message from '{client_name}': {e}")

    except websockets.exceptions.ConnectionClosedOK:
        print(f"[SERVER] Client '{client_name}' closed connection gracefully.")
    except websockets.exceptions.ConnectionClosedError as e:
        print(f"[SERVER] Client '{client_name}' closed connection unexpectedly: {e}")
    except Exception as e:
        # Catch errors during registration or connection setup
        print(f"[SERVER] FATAL error during connection for {client_name}: {e}")
    finally:
        # --- Inline Unregistration Logic ---
        if client_name and client_name in CLIENTS:
            del CLIENTS[client_name]
            print(f"[SERVER] {client_name} disconnected. Total clients: {len(CLIENTS)}")
        # ---------------------------------

async def main():
    """Sets up and runs the WebSocket server."""
    bind_host = "0.0.0.0"
    print(f"[SERVER] WebSocket server running on ws://{bind_host}:{SERVER_PORT}")
    async with websockets.serve(router, bind_host, SERVER_PORT):
        await asyncio.Future()  # Run forever

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("[SERVER] Server stopped manually.")
