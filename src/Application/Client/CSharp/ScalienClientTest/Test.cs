﻿using System.Collections.Generic;
namespace Scalien
{
    public class Test
    {
        // test entry point
        public static void Main(string[] args)
        {
            
            //Client.SetTrace(true);
            string[] nodes = { "localhost:7080" };
            Client client = new Client(nodes);
            client.SetMasterTimeout(3 * 1000);
            client.SetGlobalTimeout(10 * 1000);
            client.UseDatabase("test");
            client.UseTable("test");
            client.Set("key", "value");
            string value = client.Get("key");

            //string[] nodes = { "localhost:7080" };
            //Client client = new Client(nodes);
            //Database database = client.GetDatabase("Storage");
            //List<Quorum> quorums = client.GetQuorums();
            //Quorum quorum = quorums.ToArray()[0];
            //Table table = database.CreateTable(quorum, "sequence");
        }
    }
}
