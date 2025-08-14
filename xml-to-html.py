#
# The MIT License (MIT)
#
# Copyright (c) 2025 Scott Moreau <oreaus@gmail.com>
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#


import os
import xml.etree.ElementTree as ET

METADATA_DIR = "../../../metadata"

class PluginOption:
    def __init__(self, name, description):
        self.name = name
        self.description = description


class Plugin:
    def __init__(self, identifier, name, category, description):
        self.identifier = identifier
        self.name = name
        self.category = category
        self.description = description
        self.options = []

    def add_option(self, name, description):
        self.options.append(PluginOption(name, description))

    def get_options(self):
        return self.options
    

def generate_plugin_html(metadata_dir):
    categories = []
    plugins = []
    for filename in os.listdir(metadata_dir):
        if filename.endswith(".xml"):
            filepath = os.path.join(metadata_dir, filename)
            tree = ET.parse(filepath)
            root = tree.getroot()

            category = root.findtext(".//category")
            if category not in categories:
                categories.append(category)

            plugin_element = root.find("plugin")
            if plugin_element is not None:
                plugin_id = plugin_element.get("name")
            else:
                plugin_id = "default"
            plugin_name = root.findtext(".//_short")
            long_description = root.findtext(".//_long")

            if plugin_name:
                plugin = Plugin(plugin_id, plugin_name, category, long_description)
                for option in root.findall(".//option"):
                    option_name = option.get("name")
                    option_long_description = option.findtext("./_long")
                    plugin.add_option(option_name, option_long_description)
                plugins.append(plugin)

    categories.sort()
    plugins.sort(key=lambda x: x.name)

    plugin_doc_html = ""
    plugin_doc_html += f"""
        <div class="plugin-entry">
    """
    for category in categories:
        plugin_doc_html += f"""
            <div class="category-link">
                <a href="#{category}">{category}</a>
            </div>
            <div class="plugin-link">
            <ul>
        """
        for plugin in plugins:
            if plugin.category == category:
                plugin_doc_html += f"""
                <li>
                    <a href="#{plugin.identifier}">{plugin.name}</a>
                </li>
                """
        plugin_doc_html += f"""
            </ul>
            </div>
        """
    plugin_doc_html += f"""
        </div>
    """

    for category in categories:
        plugin_doc_html += f"""
            <div id="{category}" class="category-entry">
                <h3>{category}</h3>
            </div>
        """
        for plugin in plugins:
            if plugin.category == category:
                options_html = ""
                for option in plugin.get_options():
                    if option.name:
                        options_html += f"<li><strong>{option.name}:</strong><div class=\"option-desc\">{option.description if option.description else 'No description available.'}</div></li>"
                plugin_doc_html += f"""
                    <div id="{plugin.identifier}" class="plugin-entry">
                        <h2>{plugin.name}</h2>
                        <p class="long-description">{plugin.description}</p>
                """
                if len(plugin.get_options()):
                    plugin_doc_html += f"""
                            <h4>Options:</h4>
                            <ul>
                                {options_html}
                            </ul>
                    """
                plugin_doc_html += f"""
                    </div>
                """

    return plugin_doc_html

def generate_webpage(plugins_html):
    return f"""
        <!DOCTYPE html>
        <html>
        <head>
            <title>Wayfire Plugins</title>
            <style>
                body {{
                    font-family: sans-serif;
                    width: 50%;
                    margin: 25%;
                    margin-top: 50px;
                    background-color: #2e3440;
                    color: #d8dee9;
                    padding: 0px;
                }}
                header {{
                    text-align: center;
                    margin-bottom: 30px;
                }}
                h1 {{
                    color: #88c0d0;
                }}
                .plugin-entry {{
                    background-color: #3b4252;
                    border-radius: 5px;
                    padding: 75px;
                    margin-bottom: 75px;
                    box-shadow: 0 2px 5px rgba(0, 0, 0, 0.2);
                }}
                .plugin-entry h2 {{
                    color: #81a1c1;
                    margin-top: 0;
                }}
                .category-entry h3 {{
                    color: #a3be8c;
                    font-size: 2.0em;
                }}
                .category-link a {{
                    color: #a3be8c;
                    text-decoration: none;
                    font-size: 1.5em;
                }}
                .plugin-link a {{
                    color: #bf616a;
                    text-decoration: none;
                    font-size: 1.2em;
                }}
                .plugin-link ul {{
                    list-style-type: disc;
                    margin-left: 20px;
                }}
                .plugin-link ul li {{
                    margin-bottom: 5px;
                }}
                .plugin-entry h4 {{
                    color: #bf616a;
                    margin-bottom: 5px;
                }}
                .option-desc {{
                    width: 90%;
                }}
                .plugin-entry ul {{
                    list-style-type: disc;
                    margin-left: 20px;
                }}
                .plugin-entry ul li {{
                    margin-bottom: 5px;
                }}
                .plugin-entry ul li strong {{
                    color: #ebcb8b;
                }}
                html {{
                    scroll-behavior: smooth;
                }}
            </style>
        </head>
        <body>
            <header>
                <h1>Wayfire Plugins</h1>
            </header>
            <main>
                {plugins_html}
            </main>
        </body>
        </html>
    """

if __name__ == "__main__":
    plugins_html_content = generate_plugin_html(METADATA_DIR)
    webpage_content = generate_webpage(plugins_html_content)
    os.mkdir("docs")
    with open("docs/index.html", "w") as f:
        f.write(webpage_content)

    print("Wayfire plugins webpage generated successfully as docs/index.html")
