# Per-scene look (sky, fog, exposure, night lighting) applied to converted Unity scenes.
# Split out of unity_prefabs.py.

# The AAA look, so regenerating a scene keeps it. Every pack scene gets the physical sky, TAA, bloom, auto exposure and a
# gritty grade, with depth of field off. The playable scenes then get their own
# time of day on top; the catalog scenes (Showcase, Prefabs, Asset_list) keep the base look only.
LOOK_BASE = dict(
    skySource=2, antiAliasing='TAA', msaaSamples=1, fxaa=False, tonemapOperator=1,
    depthOfField=False,
    bloomEnabled=True, bloomThreshold=1.0, bloomKnee=0.5, bloomIntensity=0.14,
    autoExposure=True, autoExposureMinEV=-3.0, autoExposureMaxEV=3.0, autoExposureSpeedUp=1.5, autoExposureSpeedDown=1.0,
    gradeContrast=14.0, gradeSaturation=-10.0, gradeTint=0.0,
)
LOOK_DAY = dict(
    exposureEV=-0.3, gradeTemperature=2.0, gradeColorFilter=[1.0, 0.99, 0.97],
    volumetricEnabled=True, volDensity=0.004, volAnisotropy=0.7, volHeightFalloff=0.08, volDistance=96.0, volQuality=1,
)
# Dark and lamp-lit: a dim moon, the street lamps and windows carry the light, the streets are wet.
LOOK_NIGHT = dict(
    exposureEV=1.0, autoExposureMinEV=-1.0, autoExposureMaxEV=4.0, gradeTemperature=-10.0,
    gradeColorFilter=[0.9, 0.97, 1.05], bloomIntensity=0.2,
    volumetricEnabled=True, volDensity=0.008, volAnisotropy=0.6, volHeightFalloff=0.1, volDistance=64.0, volQuality=1,
    wetness=0.85, puddleAmount=0.45, puddleScale=4.0, puddleSsr=True,  # the lamps and facades in the puddles
)

LOOK_SCENES = {
    ('ModularCityAlleyPack', 'Example_Scene_Day'): LOOK_DAY,
    ('ModularCityAlleyPack', 'Example_Scene_Night'): LOOK_NIGHT,
    # Unity lit this interior with baked area lights and no sky ambient at all (Ambient Intensity 0): the open sky
    # flooding every wall evenly is what made it look unlit. The room probes' diffuse and the area fills light it now.
    ('OfficeAndPoliceStation', 'OfficePack_Example'): dict(
        exposureEV=-0.7, gradeTemperature=-2.0, gradeColorFilter=[0.97, 1.0, 1.0], volumetricEnabled=False,
        skyAmbientIntensity=0.35, ssaoIntensity=1.6),
    # ~100 real-time lamps: 2 sun/moon cascades keep the street in budget (4 cost 1.7 ms more for no visible gain).
    # Day: a stronger sun over less sky fill, so shade reads as shade (Unity's ratio was ~5:1, ours was ~2:1).
    ('SuburbNeighborhoodHousePack', 'Demoscene_Day'): dict(LOOK_DAY, shadowCascades=2, skyAmbientIntensity=0.6),
    # Night: the moon and sky fill were lifting the whole street to dusk; the exposure cap keeps dark dark.
    ('SuburbNeighborhoodHousePack', 'Demoscene_Night'): dict(
        LOOK_NIGHT, shadowCascades=1, skyAmbientIntensity=0.35, autoExposureMaxEV=2.5,
        gradeTemperature=-4.0,  # a lighter cool grade: the sodium lamps keep their amber
        ssaoEnabled=False, volQuality=0),  # under the frame budget: SSAO off, one moon cascade, low froxel fog
}
NIGHT_MOON_SKY = 0.006  # sky moonBrightness: dim, though the full high moon from the night offset is still there to see by
NIGHT_SHADOW_RES = 2048  # world shadowResolution for night scenes (moon map); 4096 default
NIGHT_CLOUD_QUALITY = 0 # sky cloudQuality 0 = Low
NIGHT_MOON_LIGHT = 0.35 # the moon's directional light (the Unity scene's own, or the template Sun standing in for it)
NIGHT_LAMP_VOLUMETRIC = 0.5  # point/spot lights' Volumetric Intensity: halves the street-lamp beams in the night fog


# Per-scene light fixes, as absolute values so re-applying the look never compounds: 'Directional' is the sun/moon, any
# other key a light name; each maps Light fields to set.
LOOK_LIGHTS = {
    ('SuburbNeighborhoodHousePack', 'Demoscene_Day'): {'Directional': {'Intensity': 3.2}},
    ('SuburbNeighborhoodHousePack', 'Demoscene_Night'): {
        'Directional': {'Intensity': 0.15},
        'Spotlight_Street_high': {'Intensity': 45.0, 'Shadow Softness': 2.5},
        'Spotlight_Street_low': {'Shadow Softness': 2.5}},
}


def apply_look(pack, stem, scene, empties):
    scene.update(LOOK_BASE)
    look = LOOK_SCENES.get((pack.name, stem))
    if look is None:
        return
    scene.update(look)
    if 'night' in stem.lower():
        scene['sky'] = dict(scene['sky'], moonBrightness=NIGHT_MOON_SKY, cloudQuality=NIGHT_CLOUD_QUALITY)
        scene['shadowResolution'] = NIGHT_SHADOW_RES
        for e in empties:
            light = e.get('Light') or {}
            if light.get('Type') == 'Directional':
                light['Intensity'] = NIGHT_MOON_LIGHT
            elif light.get('Type') in ('Point', 'Spot'):
                light['Volumetric Intensity'] = NIGHT_LAMP_VOLUMETRIC
    fixes = LOOK_LIGHTS.get((pack.name, stem), {})
    for e in empties:
        light = e.get('Light')
        if light:
            light.update(fixes.get('Directional' if light.get('Type') == 'Directional' else e['name'], {}))
    pack.counts['scenes given the AAA look preset'] += 1
