echo "drv --------"
doas kldload -v ./pci7250.ko
echo "--------"
kldstat
echo "--------"
ls /dev/din*
ls /dev/dout*
echo "--------"
echo -n "10101010" > /dev/dout0
sleep 0.5
echo -n "01010101" > /dev/dout0
sleep 0.5
echo -n "00" > /dev/dout0
sleep 0.5
for bit_device in /dev/dout0.*
do
    echo -n "1" > $bit_device
    echo "$bit_device := 1"
    sleep 0.5
done
for bit_device in /dev/dout0.*
do
    echo -n "0" > $bit_device
    echo "$bit_device := 0"
    sleep 0.5
done
echo "--------"
dmesg | grep pci7250 | tail -n 40
echo "--------"
doas kldunload -v pci7250.ko
echo "fin --------"
