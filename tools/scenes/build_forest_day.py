# ForestDay: the Forest (build_forest.py) on a clear late morning in summer. Same world entity for entity - the house,
# yard and forest are the Forest's - with only the sky, the sun and the grade changed, so rebuild the Forest first.
#
#   python tools/scenes/build_forest_day.py [<repo>]
# Physical sun at 10:30 on day 200, light cumulus and cirrus with cloud shadows, no fog or volumetric haze, neutral
# exposure and grade, gentle scene wind.
import json
import os
import sys

import build_forest as bf
import scene_kit as k

SETTINGS = {'exposureEV': 0.0, 'gradeContrast': 8.0, 'gradeSaturation': 4.0, 'gradeTemperature': 2.0,
            'shadowDistance': 120.0, 'skyAmbientIntensity': 1.0, 'volumetricEnabled': False}
SKY = {'cirrusCoverage': 0.3, 'cloudCoverage': 0.35, 'cloudShadowStrength': 0.6, 'cloudShadows': True,
       'cloudType': 0.45, 'cloudWindSpeed': 8.0, 'dayOfYear': 200.0, 'groundAlbedo': [0.18, 0.2, 0.14],
       'skyIntensity': 1.0, 'timeOfDayHours': 10.5, 'windStrength': 0.9, 'windGustiness': 0.5, 'windGustSpeed': 3.5}
SUN = {'Angular Size': 0.8, 'Color': [1.0, 0.97, 0.92], 'Intensity': 3.2, 'Shadow Softness': 0.3,
       'Volumetric Intensity': 1.0}


def main():
    repo = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', '..'))
    scenes = os.path.join(repo, 'project', 'scenes')
    with open(os.path.join(scenes, 'Forest.json'), encoding='utf-8') as f:
        d = json.load(f)
    d.update(SETTINGS)
    d['sky'].update(SKY)
    moon = next(e for e in d['empties'] if e['name'] == 'Moon' and e['parentId'] == -1)
    moon['name'] = 'Sun'
    moon['Light'].update(SUN)
    s = k.Scene({})
    s.data = {key: v for key, v in d.items() if key not in ('boxes', 'empties', 'models')}
    s.boxes, s.empties, s.models = d['boxes'], d['empties'], d['models']
    out = os.path.join(scenes, 'ForestDay.json')
    s.write(out)
    bf.write_meta(out)
    print(f'build_forest_day: -> {out}')


if __name__ == '__main__':
    main()
