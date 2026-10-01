# Data

Not committed (8+ GB). To reproduce:

```bash
cd data
curl -LO "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/12302019.NASDAQ_ITCH50.gz"   # 3.52 GB
gunzip -k 12302019.NASDAQ_ITCH50.gz                                              # 8.25 GB
curl -LO "https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf"
```

Nasdaq publishes no checksum for this date; gzip's CRC check during
decompression verifies the download. The file is a sequence of messages,
each preceded by a 2-byte big-endian length.
