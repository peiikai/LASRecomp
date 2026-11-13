"""Plain names for the entrance randomizer's doors (tools/rando/entrance_data.yml ids), for the readable spoiler.

The same name serves both sides: "Mabe Village library" is the library's door outside and the library inside.
Anything missing falls back to the id, spelled out.
"""

NAMES = {
    # Tal Tal Mountain Range / Tal Tal Heights
    'd8-west-side-exit': 'Turtle Rock, west side door', 'd8-east-side-exit': 'Turtle Rock, east side door',
    'fire-cave-north': 'Fire cave, north door', 'fire-cave-south': 'Fire cave, south door',
    'windfish-egg': "Wind Fish's egg", 'taltal-cucco-house': 'Tal Tal Heights, rooster house',
    'd7-eagles-tower': "Eagle's Tower", 'd8-turtle-rock': 'Turtle Rock',
    'taltal-phone-d8': 'Telephone booth by Turtle Rock', 'taltal-mad-batter': 'Mad Batter well, Tal Tal',
    'taltal-crossing-west': 'Tal Tal crossing cave, west door', 'taltal-crossing-east': 'Tal Tal crossing cave, east door',
    'mountain-access-entrance': 'Mountain cave, bottom door', 'mountain-access-exit': 'Mountain cave, top door',
    'mountain-access-chest-ledge': 'Mountain cave, ledge door', 'papahl-cave-lower': "Papahl's cave, lower door",
    'papahl-cave-upper': "Papahl's cave, upper door", 'taltal-bird-key-cave': 'Bird Key cave',
    'five-chest-tunnel-west': 'Five chest cave, west door', 'five-chest-tunnel-east': 'Five chest cave, east door',
    'five-chest-game-exit': 'Five chest game, exit door', 'outer-rainbow-cave-west': 'Rainbow cave, outer west door',
    'inner-rainbow-cave-west': 'Rainbow cave, inner west door', 'inner-rainbow-cave-east': 'Rainbow cave, inner east door',
    'outer-rainbow-cave-east': 'Rainbow cave, outer east door', 'taltal-east-fairy': 'Fairy fountain, Tal Tal east',
    'path-to-d7-south': "Cave to Eagle's Tower, south door", 'path-to-d7-north': "Cave to Eagle's Tower, north door",
    # Goponga Swamp / Tabahl Wasteland / Kanalet
    'goponga-cave-west': 'Goponga Swamp cave, west door', 'goponga-cave-east': 'Goponga Swamp cave, east door',
    'd2-bottle-grotto': 'Bottle Grotto', 'manbo-cave': "Manbo's cave",
    'd4-connector-west': "Cave by Angler's Tunnel, west door", 'd4-anglers-tunnel': "Angler's Tunnel",
    'd4-connector-east': "Cave by Angler's Tunnel, east door", 'damp-cave': 'Damp cave',
    'raft-return-north': 'Raft cave, north door', 'raft-return-south': 'Raft cave, south door',
    'mr-write-house': "Mr. Write's house", 'swamp-phone': 'Telephone booth, Goponga Swamp',
    'moblin-cave': 'Moblin cave', 'dampe-house': "Dampe's house", 'raft-shop': 'Rapids Ride raft shop',
    'woods-north-cave': 'Mysterious Forest, north cave', 'crazy-tracy-house': "Crazy Tracy's house",
    'kanalet-tunnel-west': 'Kanalet tunnel, west door', 'kanalet-tunnel-east': 'Kanalet tunnel, east door',
    'kanalet-phone': 'Telephone booth by Kanalet Castle', 'kanalet-castle-roof-west': 'Kanalet Castle, roof west door',
    'kanalet-castle-roof-east': 'Kanalet Castle, roof east door', 'kanalet-castle-front': 'Kanalet Castle, front door',
    'kanalet-south-cave': 'Cave south of Kanalet Castle',
    # Mysterious Forest / Koholint Prairie / Cemetery
    'woods-crossing-cave-north': 'Forest crossing cave, north door',
    'woods-crossing-cave-south': 'Forest crossing cave, south door', 'woods-mad-batter': 'Mad Batter well, forest',
    'witch-hut': "Witch's hut", 'd6-north-side-exit': 'Face Shrine, north side door',
    'graveyard-cave-west': 'Cemetery cave, west door', 'graveyard-cave-east': 'Cemetery cave, east door',
    'd0-color-dungeon': 'Color Dungeon', 'prairie-fairy': 'Fairy fountain, Koholint Prairie',
    'quadruplet-house-west': "Quadruplets' house, west door", 'quadruplet-house-east': "Quadruplets' house, east door",
    'dream-shrine': 'Dream Shrine', 'ukuku-cave-west': 'Ukuku cave, west door', 'ukuku-cave-east': 'Ukuku cave, east door',
    'prairie-phone-north': 'Telephone booth, Ukuku Prairie north', 'prairie-phone-west': 'Telephone booth, Ukuku Prairie west',
    'seashell-mansion': 'Seashell Mansion', 'd6-face-shrine': 'Face Shrine',
    'd6-connector-west': 'Cave to Face Shrine, west door', 'd6-connector-east': 'Cave to Face Shrine, east door',
    'armos-fairy': 'Fairy fountain by the Armos maze', 'rooster-grave': "Flying Rooster's grave",
    'river-crossing-cave-west': 'River crossing cave, west door', 'river-crossing-cave-east': 'River crossing cave, east door',
    'southern-face-shrine': 'Southern Face Shrine', 'armos-maze-cave': 'Armos maze cave',
    # Mabe Village
    'town-tool-shop': 'Town Tool Shop', 'mabe-kennel': "BowWow's kennel", 'madam-meowmeow-house': "Madam MeowMeow's house",
    'marin-tarin-house': "Marin and Tarin's house", 'mabe-library': 'Mabe Village library', 'ulrira-house': "Ulrira's house",
    'mabe-phone': 'Telephone booth, Mabe Village', 'trendy-game': 'Trendy Game',
    # south: Key Cavern, Martha's Bay, Animal Village, Yarna Desert
    'd3-key-cavern': 'Key Cavern', 'bay-cliff-cave-north': 'Cliff cave by the bay, north door',
    'bay-cliff-cave-plateau': 'Cliff cave by the bay, plateau door', 'bay-cliff-cave-south': 'Cliff cave by the bay, south door',
    'richard-villa': "Richard's villa", 'richard-villa-cave-exit': "Cave behind Richard's villa",
    'animal-village-rabbit-house': 'Animal Village, rabbit house', 'schule-donavitch-house': "Schule Donavitch's house",
    'christine-house': "Christine's house", 'hidden-zora-house': "Zora's house", 'animal-village-bomb-cave': 'Animal Village, bombed cave',
    'desert-lanmola-cave': 'Lanmola cave, Yarna Desert', 'd1-tail-cave': 'Tail Cave', 'mamu-cave': "Mamu's cave",
    'd5-catfishs-maw': "Catfish's Maw", 'catfish-maw-dive-east': "Catfish's Maw, east dive spot",
    'catfish-maw-dive-west': "Catfish's Maw, west dive spot", 'animal-village-phone': 'Telephone booth, Animal Village',
    'bear-chef-house': "Chef Bear's house", 'banana-sale-house': "Sale's banana house", 'bay-mad-batter': 'Mad Batter well, bay',
    'bay-batter-tunnel-north': 'Tunnel to the bay Mad Batter, north door',
    'bay-batter-tunnel-south': 'Tunnel to the bay Mad Batter, south door', 'bay-phone': "Telephone booth, Martha's Bay",
    'mermaid-statue': 'Mermaid statue', 'bay-fisherman-boat': "Fisherman's boat", 'boomerang-cave': 'Boomerang cave',
    'ghost-house': "Ghost's house",
}


def name(door):
    if door in NAMES:
        return NAMES[door]
    s = door.replace('-', ' ')
    return s[:1].upper() + s[1:]
