import sys
import re
import csv
import argparse
from datetime import datetime

# Regex patterns for parsing specific log messages
NODE_STATUS_RE = re.compile(
    r'UART_NODE_STATUS\s+node=(?P<node>\d+)\s+role=(?P<role>\w+)\s+present=(?P<present>\w+)\s+active=(?P<active>\w+)\s+temp=(?P<temp>[\d.]+)\s+C\s+age_s=(?P<age_s>\d+)\s+q=(?P<q>\d+)%\s+rssi=(?P<rssi>-?\d+)\s+fault=(?P<fault>\w+)\s+rejected=(?P<rejected>\w+)\s+round=(?P<round>\d+)'
)

CONSENSUS_RE = re.compile(
    r'CONSENSUS\s+round=(?P<round>\d+)\s+temp=(?P<temp>[\d.]+)\s+C\s+nodes=(?P<nodes>0x[0-9a-fA-F]+)\s+reject=(?P<reject>0x[0-9a-fA-F]+)\s+fault=(?P<fault>0x[0-9a-fA-F]+)\s+quorum=(?P<quorum>\w+)\s+status=(?P<status>\w+)'
)

def main():
    parser = argparse.ArgumentParser(description="Parse and log KryOS_MESH monitor data in real-time.")
    parser.add_argument("--out-nodes", default="mesh_nodes.csv", help="CSV file for node status data")
    parser.add_argument("--out-consensus", default="mesh_consensus.csv", help="CSV file for consensus data")
    args = parser.parse_args()

    # Open CSV files for appending
    with open(args.out_nodes, 'a', newline='') as f_nodes, \
         open(args.out_consensus, 'a', newline='') as f_cons:
        
        nodes_writer = csv.DictWriter(f_nodes, fieldnames=['timestamp', 'node', 'role', 'present', 'active', 'temp_c', 'age_s', 'quality_pct', 'rssi', 'fault', 'rejected', 'round'])
        cons_writer = csv.DictWriter(f_cons, fieldnames=['timestamp', 'round', 'temp_c', 'nodes_mask', 'reject_mask', 'fault_mask', 'quorum', 'status'])
        
        # Write headers if files are empty
        if f_nodes.tell() == 0:
            nodes_writer.writeheader()
        if f_cons.tell() == 0:
            cons_writer.writeheader()

        print(f"Listening for KryOS_MESH logs...")
        print(f"Saving node data to: {args.out_nodes}")
        print(f"Saving consensus data to: {args.out_consensus}")
        print("Waiting for data (press Ctrl+C to stop)...")

        try:
            # Read line by line from Standard Input as it comes
            for line in sys.stdin:
                line = line.strip()
                if not line:
                    continue
                
                # Print to standard output so you can still see the logs in terminal
                print(line)
                
                if "KryOS_MESH" not in line:
                    continue

                timestamp = datetime.now().isoformat()

                # Match Node Status
                m_node = NODE_STATUS_RE.search(line)
                if m_node:
                    nodes_writer.writerow({
                        'timestamp': timestamp,
                        'node': m_node.group('node'),
                        'role': m_node.group('role'),
                        'present': m_node.group('present'),
                        'active': m_node.group('active'),
                        'temp_c': m_node.group('temp'),
                        'age_s': m_node.group('age_s'),
                        'quality_pct': m_node.group('q'),
                        'rssi': m_node.group('rssi'),
                        'fault': m_node.group('fault'),
                        'rejected': m_node.group('rejected'),
                        'round': m_node.group('round')
                    })
                    f_nodes.flush()
                    continue

                # Match Consensus
                m_cons = CONSENSUS_RE.search(line)
                if m_cons:
                    cons_writer.writerow({
                        'timestamp': timestamp,
                        'round': m_cons.group('round'),
                        'temp_c': m_cons.group('temp'),
                        'nodes_mask': m_cons.group('nodes'),
                        'reject_mask': m_cons.group('reject'),
                        'fault_mask': m_cons.group('fault'),
                        'quorum': m_cons.group('quorum'),
                        'status': m_cons.group('status')
                    })
                    f_cons.flush()
                    continue

        except KeyboardInterrupt:
            print("\nStopped logging.")

if __name__ == "__main__":
    main()
