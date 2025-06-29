using System.Net;
using System.Net.WebSockets;
using System.Text;

namespace Networking.Server;

class Server
{
    private int count = 0;
    private Router router;

    public Server(Router router)
    {
        this.router = router;
    }

    //### Starting the server
    // Using HttpListener is reasonably straightforward. Start the listener and run a loop that receives and processes incoming WebSocket connections.
    // Each iteration of the loop "asynchronously waits" for the next incoming request using the `GetContextAsync` extension method (defined below).
    // If the request is for a WebSocket connection then pass it on to `ProcessRequest` - otherwise set the status code to 400 (bad request).
    public async void Start(string listenerPrefix)
    {
        HttpListener listener = new HttpListener();
        listener.Prefixes.Add(listenerPrefix);
        listener.Start();
        Console.WriteLine("Listening...");

        while (true)
        {
            HttpListenerContext listenerContext = await listener.GetContextAsync();
            if (listenerContext.Request.IsWebSocketRequest)
            {
                ProcessRequest(listenerContext);
            }
            else
            {
                listenerContext.Response.StatusCode = 400;
                listenerContext.Response.Close();
            }
        }
    }

    //### Accepting WebSocket connections
    // Calling `AcceptWebSocketAsync` on the `HttpListenerContext` will accept the WebSocket connection, sending the required 101 response to the client
    // and return an instance of `WebSocketContext`. This class captures relevant information available at the time of the request and is a read-only
    // type - you cannot perform any actual IO operations such as sending or receiving using the `WebSocketContext`. These operations can be
    // performed by accessing the `System.Net.WebSocket` instance via the `WebSocketContext.WebSocket` property.
    private async void ProcessRequest(HttpListenerContext listenerContext)
    {
        WebSocketContext? webSocketContext = null;
        try
        {
            // When calling `AcceptWebSocketAsync` the negotiated subprotocol must be specified. This sample assumes that no subprotocol
            // was requested.
            webSocketContext = await listenerContext.AcceptWebSocketAsync(subProtocol: null);
            Interlocked.Increment(ref count);
            Console.WriteLine("Processed: {0}", count);
        }
        catch (Exception e)
        {
            // The upgrade process failed somehow. For simplicity lets assume it was a failure on the part of the server and indicate this using 500.
            listenerContext.Response.StatusCode = 500;
            listenerContext.Response.Close();
            Console.WriteLine("Exception: {0}", e);
            return;
        }

        if (webSocketContext == null)
        {
            return;
        }

        WebSocket webSocket = webSocketContext.WebSocket;

        try
        {
            //### Receiving
            // Define a receive buffer to hold data received on the WebSocket connection. The buffer will be reused as we only need to hold on to the data
            // long enough to send it back to the sender.
            byte[] receiveBuffer = new byte[1024];

            // While the WebSocket connection remains open run a simple loop that receives data and sends it back.
            while (webSocket.State == WebSocketState.Open)
            {
                // The first step is to begin a receive operation on the WebSocket. `ReceiveAsync` takes two parameters:
                //
                // * An `ArraySegment` to write the received data to.
                // * A cancellation token. In this example we are not using any timeouts so we use `CancellationToken.None`.
                //
                // `ReceiveAsync` returns a `Task<WebSocketReceiveResult>`. The `WebSocketReceiveResult` provides information on the receive operation that was just
                // completed, such as:
                //
                // * `WebSocketReceiveResult.MessageType` - What type of data was received and written to the provided buffer. Was it binary, utf8, or a close message?
                // * `WebSocketReceiveResult.Count` - How many bytes were read?
                // * `WebSocketReceiveResult.EndOfMessage` - Have we finished reading the data for this message or is there more coming?
                WebSocketReceiveResult receiveResult = await webSocket.ReceiveAsync(
                    new ArraySegment<byte>(receiveBuffer),
                    CancellationToken.None
                );

                switch (receiveResult.MessageType)
                {
                    case WebSocketMessageType.Close:
                    case WebSocketMessageType.Text:
                        await webSocket.CloseAsync(
                            WebSocketCloseStatus.NormalClosure,
                            "",
                            CancellationToken.None
                        );
                        break;
                    case WebSocketMessageType.Binary:
                        // Get the actual bytes received (not the entire buffer)
                        var receivedBytes = new byte[receiveResult.Count];
                        Array.Copy(receiveBuffer, receivedBytes, receiveResult.Count);

                        BinaryReader reader = new BinaryReader(new MemoryStream(receivedBytes));
                        string hexString = BitConverter.ToString(receivedBytes).Replace("-", " ");
                        Console.WriteLine($"Binary message received: {receiveResult.Count} bytes");
                        Console.WriteLine($"  Hex: {hexString}");

                        // Convert to hex string
                        // string hexString = BitConverter.ToString(receivedBytes).Replace("-", " ");

                        // // Convert to readable string (if possible)
                        // string asciiString = Encoding.ASCII.GetString(receivedBytes);

                        // Console.WriteLine($"Binary message received: {receiveResult.Count} bytes");
                        // Console.WriteLine($"  Hex: {hexString}");
                        // Console.WriteLine($"  ASCII: {asciiString}");

                        // ArraySegment<byte> response = router.HandleMessage(receiveBuffer);

                        await webSocket.SendAsync(
                            new ArraySegment<byte>(receiveBuffer, 0, receiveBuffer.Count()),
                            // response,
                            WebSocketMessageType.Binary,
                            true,
                            CancellationToken.None
                        );
                        break;
                }

                // The echo operation is complete. The loop will resume and `ReceiveAsync` is called again to wait for the next data frame.
            }
        }
        catch (Exception e)
        {
            // Just log any exceptions to the console. Pretty much any exception that occurs when calling `SendAsync`/`ReceiveAsync`/`CloseAsync` is unrecoverable in that it will abort the connection and leave the `WebSocket` instance in an unusable state.
            Console.WriteLine("Exception: {0}", e);
        }
        finally
        {
            // Clean up by disposing the WebSocket once it is closed/aborted.
            if (webSocket != null)
                webSocket.Dispose();
        }
    }
}
