using API;

//## The program entry point
// Passes an HttpListener prefix for the server to listen on. The prefix 'http://+:80/wsDemo/' indicates that the server should listen on
// port 80 for requests to wsDemo (e.g. http://localhost/wsDemo). For more information on HttpListener prefixes see [MSDN](http://msdn.microsoft.com/en-us/library/system.net.httplistener.aspx).
class Program
{
    static void Main(string[] args)
    {
        Router router = new Router();
        Server server = new Server(router);
        server.Start("http://localhost:6969/");
        Console.WriteLine("Press any key to exit...");
        Console.ReadKey();
    }
}
