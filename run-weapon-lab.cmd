@echo off
rem The Weapon Lab: the editor on the WeaponLab scene, the Scene view (the world body: what everyone else
rem sees) beside the Game view (your own), and the Weapon Lab panel. Builds and launches like run-editor.cmd.
set TARTARUS_WEAPON_LAB=1
call "%~dp0run-editor.cmd" %*
