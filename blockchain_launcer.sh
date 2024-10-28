#!/bin/bash

if [[ "$EUID" = 0 ]]
then
	echo "Pulling mtacoin-server image from repo.."
	docker image pull commissi0n/mtacoin-server:1.0 &>/dev/null
	
	echo "Pulling mtacoin-miner image from repo.."
	docker image pull commissi0n/mtacoin-miner:1.0 &>/dev/null
	
	echo "Running server container.."
	server_container_id=$(docker run -d -v ./conf/mta/:/mnt/mta commissi0n/mtacoin-server:1.0)
	echo "Server container ID - ${server_container_id}"
	
	echo "Running miner #1 container.."
	miner_1_container_id=$(docker run -d -v ./conf/mta/:/mnt/mta commissi0n/mtacoin-miner:1.0)
	echo "Miner #1 container ID - ${miner_1_container_id}"
	
	echo "Running miner #2 container.."
	miner_2_container_id=$(docker run -d -v ./conf/mta/:/mnt/mta commissi0n/mtacoin-miner:1.0)
	echo "Miner #2 container ID - ${miner_2_container_id}"
	
	echo "Running miner #3 container.."
	miner_3_container_id=$(docker run -d -v ./conf/mta/:/mnt/mta commissi0n/mtacoin-miner:1.0)
	echo "Miner #3 container ID - ${miner_3_container_id}"
	
	echo "Running miner #4 container.."
	miner_4_container_id=$(docker run -d -v ./conf/mta/:/mnt/mta commissi0n/mtacoin-miner:1.0)
	echo "Miner #4 container ID - ${miner_4_container_id}"
	
	echo "Tail the server's container blockchain log (/var/log/mtacoin.log) from the beginning? (y/n)"
	
	while :
	do
		read -r input
		case "$input" in
		y|Y)
			echo "Tailing server's log.."
			#tail from beginning of file
			docker exec -it ${server_container_id} tail -f -n +1 /var/log/mtacoin.log
			break
			;;
		n|N)
			break
			;;
		*)
			echo "Invalid input. Try again."
			;;
		esac
	done
else
	echo "ERROR: Must run with root privileges"
fi
