all: reliable_sender reliable_receiver

reliable_sender: reliable_sender.c 
	gcc reliable_sender.c -g -o reliable_sender -lpthread

reliable_receiver: reliable_receiver.c 
	gcc reliable_receiver.c -o reliable_receiver -lpthread

clean: 
	rm reliable_receiver reliable_sender












