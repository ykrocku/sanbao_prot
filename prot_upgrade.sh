#!/system/bin/sh


cd /mnt/obb
busybox mv package.bin package.tar.gz
busybox tar xzf package.tar.gz

if [ -f adasProt ]; then
    echo "update adasProt..."
    busybox chmod 0755 adasProt
    busybox cp adasProt /data/adasProt_new
    busybox mv /data/adasProt_new /data/adasProt
else
    echo "adasProt not exsist"
fi

if [ -f dmsProt ]; then
    echo "update dmsProt..."
    busybox chmod 0755 dmsProt
    busybox cp dmsProt /data/dmsProt_new
    busybox mv /data/dmsProt_new /data/dmsProt
else
    echo "dmsProt not exsist"
fi
