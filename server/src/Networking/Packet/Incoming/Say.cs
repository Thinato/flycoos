using System.Text;

namespace Networking.Packet.Incoming;

class Say : Packet
{
    public override PacketID Id => PacketID.SAY;

    public override Packet CreateInstance() => new Say();

    public byte ChatId { get; set; }
    public string? Text { get; set; }

    protected override void Read(BinaryReader reader)
    {
        ChatId = reader.ReadByte();
        Text = Encoding.UTF8.GetString(reader.ReadBytes(256));
    }

    protected override void Write(BinaryWriter writer)
    {
        writer.Write(ChatId);
        writer.Write(Encoding.UTF8.GetBytes(Text ?? ""));
    }
}
