namespace Networking.Packet.Outgoing;

class Say : Packet
{
    public override PacketID Id => PacketID.SAY;

    public override Packet CreateInstance() => new Say();

    protected override void Read(BinaryReader reader)
    {
        throw new NotImplementedException();
    }

    protected override void Write(BinaryWriter writer)
    {
        throw new NotImplementedException();
    }
}
