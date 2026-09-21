# Домашняя работа

0. Таблица

| Тип                       | 	Условие             | 	Фильтр	                                         | Пояснение                                                                    |
|---------------------------|----------------------|--------------------------------------------------|------------------------------------------------------------------------------|
| Первый фрагмент           | 	Offset = 0, MF = 1	 | ip[6:2] & 0x1FFF = 0 and ip[6:2] & 0x2000 != 0	  | Смещение будет нулевое + флаг "Еще фрагменты" включен                        |
| Средний фрагмент          | 	Offset > 0, MF = 1	 | ip[6:2] & 0x1FFF != 0 and ip[6:2] & 0x2000 != 0	 | Смещение будет ненулевое + флаг  "Еще фрагменты" включен                     |
| Последний фрагмент        | 	Offset > 0, MF = 0	 | ip[6:2] & 0x1FFF != 0 and ip[6:2] & 0x2000 = 0	  | Смещение будет ненулевое + флаг "Еще фрагменты" выключен - конец Дейтаграммы |
| Нефрагментированный пакет | 	Offset = 0, MF = 0	 | ip[6:2] & 0x1FFF = 0 and ip[6:2] & 0x2000 = 0	   | Пакет не в фрагментации, а смещение нулевое, флаг "Еще фрагменты" выключен   |

1. Захват и поиск по трафику

```bash
sudo tcpdump -i any -w networks_lw2_1.pcap 'tcp[tcpflags] & (tcp-syn|tcp-ack) != 0'
sudo tcpdump -r networks_lw2_1.pcap -nn 'tcp[tcpflags] & (tcp-syn|tcp-ack) == tcp-syn or tcp[tcpflags] & (tcp-syn|tcp-ack) == (tcp-syn|tcp-ack)'
```

2. Захват и поиск по трафику

```bash
sudo tcpdump -i any -nn -w networks_lw2_2.pcap 'udp port 53 or tcp port 53'
sudo tcpdump -r networks_lw2_2.pcap -nn -vv 
```

3. Захват и поиск по трафику

```bash
sudo tcpdump -i any -nn -w networks_lw2_3.pcap icmp
sudo tcpdump -r networks_lw2_3.pcap -nn 'icmp[icmptype] == icmp-echo'
```

4. Захват и поиск по трафику

```bash
sudo tcpdump -i any -s 0 -w networks_lw2_4.pcap
sudo tcpdump -r networks_lw2_4.pcap -nn -q -e | awk '{print length($0), $0}' | sort -rn | head -5
```
