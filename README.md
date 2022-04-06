# Progetto di Sistemi Operativi Avanzati

## Montaggio del modulo
Per generare il *kernel object* è necessario posizionarsi nella directory `multiflow_device_driver`, contenente il codice sorgente `multiflow_device_driver.c` e le strutture dati usate (`structs.h`) ed eseguire il comando
```
make all
```
Per montare il modulo così generato nel kernel Linux è necessario eseguire il comando
```
sudo insmod multiflow_device_driver.ko
```
Per smontare il modulo è necessario eseguire il comando
```
sudo rmmod multiflow_device_driver
```
È possibile rimuovere i file generati da una precedente compilazione mediante il comando 
```
make clean
```

## Creazione dei device file
Prima di procedere con la creazione dei device file si ha bisogno di conoscere il major number associato al driver dal Sistema Operativo. Per recuperare tale informazione si utilizza il comando 
```
sudo dmesg
```
Ottenuto il major number, si possono creare il numero voluto di device file (massimo 128) compilando il sorgente `device_creator.c`, presente nella cartella `user`, e lanciando l'eseguibile così ottenuto. I comandi da utilizzare sono i seguenti
```
make creator
sudo ./device_creator path major minor
```
dove:
- `path` è il nome dei device file;
- `major` è l'identificato del driver;
- `minor` è il numero dei device file da creare, a cui viene associato un minor number crescente a partire da 0.

## Applicazione user
All'interno della directory `user` è anche presente un applicazione che permette di provare il modulo montato, interagendo con i device istanziati in precedenza o creandone dei nuovi. Per eseguire l'applicativo user è necessario eseguire i comandi
```
make user
sudo ./user
```
