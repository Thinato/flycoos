using System.Text;

namespace Networking.Packet;

public abstract class Packet
{
    public abstract PacketID Id { get; }

    public abstract Packet CreateInstance();

    public override string ToString()
    {
        var result = new StringBuilder("{");
        var props = GetType().GetProperties();
        for (int i = 0; i < props.Length; i++)
        {
            if (i != 0)
            {
                result.Append(", ");
            }
            result.AppendFormat("{0}: {1}", props[i].Name, props[i].GetValue(this, null));
        }
        result.Append("}");
        return result.ToString();
    }

    protected abstract void Read(BinaryReader reader);
    protected abstract void Write(BinaryWriter writer);
}
