# studio-setup

Create the studio roster members that the Skywalker subagents act as (creative director, level designer, environment artist, gameplay programmer, technical artist, sound designer, playtester, critic).

Prepare the project's studio roster. Argument (optional): **the arguments the user typed after the command**

1. `studio_agent_list {}` to see who already exists. Never duplicate or overwrite existing members' profiles (the `studio_agent_define` call merges, but ask before changing a name or persona).
2. If the argument names a team template, call `studio_team_template {template}` and report the ids; the subagents then act as those ids (`as:"<id>"`) matching their roles.
3. Otherwise define the missing members that this plugin's subagents act as (id equals the role): `creative_director`, `level_designer`, `environment_artist`, `gameplay_programmer`, `technical_artist`, `sound_designer`, `playtester`, `critic`. Example:
   `studio_agent_define {id:"level_designer", name:"Mira", role:"level_designer", focus:"layout and pacing", reports_to:"creative_director"}`. Give `playtester` a persona (`playtest:{policy:"goal_seeker", reaction_time:0.35, skill:0.6, curiosity:0.5, patience:30}`), set the critic and playtester to `autonomy:"observe"`, and make the others `autonomous`.
4. `studio_overview {}` to confirm. Print the roster with ids, roles and which subagent (`skywalker:<name>` in Claude Code) maps to each, and say how to start (`playtest-loop`).
