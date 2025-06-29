namespace Networking.Server;

class ClientPool
{
    private readonly List<Client> _clients = new();

    public void AddClient(Client client)
    {
        _clients.Add(client);
    }

    public void RemoveClient(Client client)
    {
        _clients.Remove(client);
    }

    public Client? GetClient(int id)
    {
        return _clients.FirstOrDefault(c => c.Id == id);
    }
}
