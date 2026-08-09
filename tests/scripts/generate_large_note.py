#!/usr/bin/env python3
import json

def main():
    payload = {'title': 'Large Note', 'content': 'word ' * 100005}
    print(json.dumps(payload))

if __name__ == "__main__":
    main()
