﻿using Lidgren.Network;
using Skyrim.API;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Text;
using System.Threading.Tasks;

namespace Skyrim.Server.Internals
{
    class MasterServer
    {
        IPEndPoint masterServerEndpoint;
        float lastRegistered = -60.0f;
        GameServer server = null;

        public MasterServer(GameServer pServer)
        {
            server = pServer;
            masterServerEndpoint = NetUtility.Resolve("localhost", Skyrim.API.MasterServer.MasterServerPort);
        }

        public void Update()
        {
            if (NetTime.Now > lastRegistered + 60)
            {
                NetOutgoingMessage regMsg = server.Server.CreateMessage();
                regMsg.Write((byte)MasterServerMessageType.RegisterHost);
                IPAddress mask;
                IPAddress adr = NetUtility.GetMyAddress(out mask);
                regMsg.Write(server.Server.UniqueIdentifier);
                regMsg.Write(server.Name);
                regMsg.Write((UInt16)server.Server.ConnectionsCount);
                regMsg.Write((UInt16)32);
                regMsg.Write(new IPEndPoint(adr, 14242));
                Console.WriteLine("Sending registration to master server");
                server.Server.SendUnconnectedMessage(regMsg, masterServerEndpoint);
                lastRegistered = (float)NetTime.Now;
            }
        }
    }
}
