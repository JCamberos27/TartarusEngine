using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;
using System.Text.Json;
using System.Xml.Linq;
using System.Text;

namespace Tartarus.CodeAnalysis;

// A separate, short-lived compiler process. Reads source/metadata only; never executes project assemblies.
public static class Program
{
    public sealed record Buffer(string Path, string Text);
    public sealed record Request(string Root, string File, string Runtime, Buffer[] Buffers, int Line, int Column,
        string Mode = "analyze", string NewName = "");
    public sealed record Place(string Path, int Line, int Column, int EndLine, int EndColumn);
    public sealed record Problem(string Severity, string Code, string Message, Place? Location);
    public sealed record Suggestion(string Name, string Kind, string Detail);
    public sealed record Edit(string Path, int Start, int Length, string Text, string Before);
    public sealed record Highlight(int Line, int Start, int Length, string Kind);
    public sealed record Result(Problem[] Diagnostics, Suggestion[] Completions, string Info, Place[] Definitions,
        Place[] References, string? Formatted, Edit[] Edits, string Error = "", Highlight[]? Highlights = null);
    static readonly JsonSerializerOptions Json = new() { PropertyNameCaseInsensitive = true, PropertyNamingPolicy = JsonNamingPolicy.CamelCase };
    sealed class ApiDocumentation : DocumentationProvider {
        readonly Dictionary<string,string> members;
        readonly string path;
        public ApiDocumentation(string file){path=file;members=XDocument.Load(file).Descendants("member").ToDictionary(m=>(string)m.Attribute("name")!,m=>m.ToString());}
        protected override string GetDocumentationForSymbol(string documentationMemberID,System.Globalization.CultureInfo preferredCulture,CancellationToken cancellationToken=default)
            => members.GetValueOrDefault(documentationMemberID,"");
        public override bool Equals(object? other)=>other is ApiDocumentation doc && doc.path==path;
        public override int GetHashCode()=>path.GetHashCode();
    }
    static bool EditorFile(string path) => path.Replace('\\','/').Split('/').Contains("Editor",StringComparer.OrdinalIgnoreCase);
    static Place? Location(Location location) {
        if(!location.IsInSource) return null;
        var span=location.GetLineSpan();
        return new(span.Path,span.StartLinePosition.Line+1,span.StartLinePosition.Character+1,span.EndLinePosition.Line+1,span.EndLinePosition.Character+1);
    }
    static string Detail(ISymbol symbol) => symbol.ToDisplayString(SymbolDisplayFormat.MinimallyQualifiedFormat);
    static string Documentation(ISymbol symbol) {
        var xml=symbol.GetDocumentationCommentXml();
        if(string.IsNullOrWhiteSpace(xml))return "";
        try {return string.Join(" ",XElement.Parse(xml).DescendantNodes().OfType<XText>().Select(t=>t.Value.Trim()).Where(t=>t.Length>0));}
        catch {return "";}
    }
    static ISymbol? Symbol(SemanticModel model,SyntaxToken token) {
        for(var node=token.Parent;node!=null;node=node.Parent) {
            var info=model.GetSymbolInfo(node);
            if(info.Symbol!=null)return info.Symbol;
            var declared=model.GetDeclaredSymbol(node);
            if(declared!=null)return declared;
            if(node is StatementSyntax or MemberDeclarationSyntax)break;
        }
        return null;
    }
    static Highlight[] Highlights(SemanticModel model,SyntaxNode root,SourceText text) {
        var highlights=new List<Highlight>();
        foreach(var token in root.DescendantTokens().Where(t=>t.IsKind(SyntaxKind.IdentifierToken))) {
            var node=token.Parent;ISymbol? symbol=null;
            if(node is IdentifierNameSyntax or GenericNameSyntax) {
                var info=model.GetSymbolInfo(node);symbol=info.Symbol ?? info.CandidateSymbols.FirstOrDefault();
                if(node is IdentifierNameSyntax name)symbol=model.GetAliasInfo(name)?.Target ?? symbol;
            } else if(node is MemberDeclarationSyntax or VariableDeclaratorSyntax or ParameterSyntax or TypeParameterSyntax or SingleVariableDesignationSyntax)
                symbol=model.GetDeclaredSymbol(node);
            string? kind=symbol switch {
                ITypeSymbol => "type",
                IMethodSymbol => "method",
                IPropertySymbol => "property",
                IFieldSymbol field when field.ContainingType.TypeKind==TypeKind.Enum => "enumMember",
                IFieldSymbol => "field",
                IEventSymbol => "property",
                IParameterSymbol => "parameter",
                ILocalSymbol or IRangeVariableSymbol => "local",
                INamespaceSymbol => "namespace",
                _ => null
            };
            if(kind==null)continue;
            var line=text.Lines.GetLineFromPosition(token.SpanStart);
            int start=Encoding.UTF8.GetByteCount(text.ToString(TextSpan.FromBounds(line.Start,token.SpanStart)));
            int length=Encoding.UTF8.GetByteCount(token.Text);
            highlights.Add(new(line.LineNumber,start,length,kind));
        }
        return highlights.ToArray();
    }
    public static Result Analyze(Request request)
    {
        var active=Path.GetFullPath(request.File);
        var buffers=request.Buffers.ToDictionary(b=>Path.GetFullPath(b.Path),b=>b.Text,StringComparer.OrdinalIgnoreCase);
        bool editor=EditorFile(active);
        var paths=Directory.EnumerateFiles(Path.Combine(request.Root,"assets"),"*.cs",SearchOption.AllDirectories)
            .Where(p=>EditorFile(p)==editor).Select(Path.GetFullPath).ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach(var p in buffers.Keys.Where(p=>EditorFile(p)==editor))paths.Add(p);
        paths.Add(active);
        var options=new CSharpParseOptions(LanguageVersion.CSharp14,preprocessorSymbols:["TRACE","NET","NET10_0","NETCOREAPP","NET10_0_OR_GREATER","NET9_0_OR_GREATER","NET8_0_OR_GREATER"]);
        var trees=paths.Select(p=>CSharpSyntaxTree.ParseText(buffers.GetValueOrDefault(p) ?? File.ReadAllText(p),options,p)).ToArray();
        var implicitUsings=CSharpSyntaxTree.ParseText("global using System; global using System.Collections.Generic; global using System.IO; global using System.Linq; global using System.Net.Http; global using System.Threading; global using System.Threading.Tasks;",options);
        var refs=((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!).Split(Path.PathSeparator)
            .Where(p=>!Path.GetFileName(p).StartsWith("Microsoft.CodeAnalysis",StringComparison.Ordinal))
            .Where(p=>Path.GetFileName(p)!="Tartarus.CodeAnalysis.dll")
            .Select(p=>MetadataReference.CreateFromFile(p)).ToList();
        string apiXml=Path.ChangeExtension(request.Runtime,".xml");
        refs.Add(MetadataReference.CreateFromFile(request.Runtime,documentation:File.Exists(apiXml)?new ApiDocumentation(apiXml):null));
        var compilation=CSharpCompilation.Create(editor?"Tartarus.Editor":"Tartarus.Gameplay",trees.Append(implicitUsings),refs,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary,allowUnsafe:!editor,nullableContextOptions:NullableContextOptions.Enable));
        var tree=trees.Single(t=>string.Equals(t.FilePath,active,StringComparison.OrdinalIgnoreCase));
        var text=tree.GetText();var root=tree.GetRoot();
        var line=text.Lines[Math.Clamp(request.Line-1,0,text.Lines.Count-1)];
        int position=Math.Min(line.End,line.Start+Math.Max(0,request.Column-1));
        var model=compilation.GetSemanticModel(tree);
        var token=root.FindToken(Math.Clamp(position>0?position-1:0,0,Math.Max(0,text.Length-1)));
        var symbol=Symbol(model,token);
        int start=position;while(start>line.Start && (char.IsLetterOrDigit(text[start-1]) || text[start-1]=='_'))--start;
        string prefix=text.ToString(TextSpan.FromBounds(start,position));
        INamespaceOrTypeSymbol? container=null;bool? staticOnly=null;
        var member=token.Parent?.AncestorsAndSelf().OfType<MemberAccessExpressionSyntax>().FirstOrDefault();
        if(start>line.Start && text[start-1]=='.' && member!=null) {
            var receiver=model.GetSymbolInfo(member.Expression).Symbol;
            container=receiver as INamespaceOrTypeSymbol ?? model.GetTypeInfo(member.Expression).Type;
            staticOnly=receiver is INamespaceOrTypeSymbol;
        }
        var candidates=model.LookupSymbols(Math.Min(position,Math.Max(0,text.Length)),container,includeReducedExtensionMethods:true)
            .Where(s=>!s.IsImplicitlyDeclared && s.CanBeReferencedByName && s.Name.StartsWith(prefix,StringComparison.OrdinalIgnoreCase)
                && (staticOnly!=true || s.IsStatic || s is INamespaceOrTypeSymbol));
        var completions=candidates.GroupBy(s=>s.Name).OrderBy(g=>g.Key).Take(250)
            .Select(g=>new Suggestion(g.Key,g.First().Kind.ToString(),string.Join("\n",g.Take(8).Select(Detail)))).ToList();
        if(container==null)foreach(var keyword in new[]{"public","private","protected","internal","class","sealed","override","void","float","int","bool","string","var","new","return","if","else","foreach","using","namespace","async","await","null","true","false"})
            if(keyword.StartsWith(prefix,StringComparison.OrdinalIgnoreCase))completions.Add(new(keyword,"Keyword",keyword));
        var problems=compilation.GetDiagnostics().Where(d=>d.Severity is DiagnosticSeverity.Error or DiagnosticSeverity.Warning).Take(400)
            .Select(d=>new Problem(d.Severity.ToString(),d.Id,d.GetMessage(),Location(d.Location))).ToArray();
        var definitions=symbol?.Locations.Select(Location).OfType<Place>().ToArray() ?? [];
        string info=symbol==null?"":Detail(symbol)+"\n"+Documentation(symbol);
        // Include overload signatures at an invocation even while arguments are incomplete.
        var invocation=token.Parent?.AncestorsAndSelf().OfType<InvocationExpressionSyntax>().FirstOrDefault();
        if(invocation!=null) {
            var overloads=model.GetMemberGroup(invocation.Expression);
            if(overloads.Length>0)info=string.Join("\n",overloads.Take(12).Select(Detail))+"\n"+(symbol==null?"":Documentation(symbol));
        }
        var references=new List<Place>();var edits=new List<Edit>();
        if(request.Mode is "references" or "rename" && symbol!=null) {
            if(request.Mode=="rename" && (!SyntaxFacts.IsValidIdentifier(request.NewName) || SyntaxFacts.GetKeywordKind(request.NewName)!=SyntaxKind.None))
                return new(problems,completions.ToArray(),info,definitions,[],null,[],"Choose a valid non-keyword C# identifier.");
            if(request.Mode=="rename" && !symbol.Locations.Any(l=>l.IsInSource))
                return new(problems,completions.ToArray(),info,definitions,[],null,[],"Referenced assembly symbols cannot be renamed.");
            foreach(var source in trees) {
                var semantics=compilation.GetSemanticModel(source);var sourceText=source.GetText().ToString();
                foreach(var candidate in source.GetRoot().DescendantTokens().Where(t=>t.IsKind(SyntaxKind.IdentifierToken) && t.ValueText==symbol.Name)) {
                    if(!SymbolEqualityComparer.Default.Equals(Symbol(semantics,candidate)?.OriginalDefinition,symbol.OriginalDefinition))continue;
                    if(Location(candidate.GetLocation()) is Place place)references.Add(place);
                    if(request.Mode=="rename")edits.Add(new(source.FilePath,candidate.SpanStart,candidate.Span.Length,request.NewName,sourceText));
                }
            }
        }
        string? formatted=request.Mode=="format" && !tree.GetDiagnostics().Any(d=>d.Severity==DiagnosticSeverity.Error)
            ? root.NormalizeWhitespace("    ","\n").ToFullString()+"\n" : null;
        return new(problems,completions.ToArray(),info.Trim(),definitions,references.ToArray(),formatted,edits.ToArray(),Highlights:Highlights(model,root,text));
    }
    public static int Main(string[] args) {
        if(args.Length==2 && args[0]=="--self-test")return SelfTest(args[1]);
        if(args.Length!=2)return 2;
        try {
            var request=JsonSerializer.Deserialize<Request>(File.ReadAllText(args[0]),Json)!;
            var result=Analyze(request);File.WriteAllText(args[1],JsonSerializer.Serialize(result,Json));return 0;
        } catch(Exception e) {
            File.WriteAllText(args[1],JsonSerializer.Serialize(new Result([],[],"",[],[],null,[],e.Message),Json));return 1;
        }
    }
    static int SelfTest(string directory) {
        Directory.CreateDirectory(Path.Combine(directory,"assets"));directory=Path.GetFullPath(directory);
        string runtime=Path.GetFullPath(Path.Combine(AppContext.BaseDirectory,"../Tartarus.Runtime.dll"));
        string file=Path.Combine(directory,"assets","Probe.cs"),other=Path.Combine(directory,"assets","Consumer.cs");
        string source="using Tartarus;\n// Unicode: 🎯\npublic sealed class Probe : Script {\n public float Speed=1;\n public override void Update(float dt) { Engine.Position(Entity); Speed += dt; }\n}\n";
        File.WriteAllText(file,source);File.WriteAllText(other,"public class Consumer { public float Read(Probe p) => p.Speed; }\r\n");
        int checks=0,failures=0;
        void Check(bool condition,string label){++checks;if(!condition){++failures;Console.WriteLine("FAIL "+label);}}
        Request Query(string text,int offset,string mode="analyze",string name="") {
            var parsed=SourceText.From(text);var point=parsed.Lines.GetLinePosition(offset);
            return new(directory,file,runtime,[new(file,text)],point.Line+1,point.Character+1,mode,name);
        }
        var correct=Analyze(Query(source,source.IndexOf("Speed +=",StringComparison.Ordinal)+3));
        Check(!correct.Diagnostics.Any(p=>p.Severity=="Error"),"valid project including implicit usings and engine reference");
        Check(correct.Info.Contains("Speed",StringComparison.Ordinal),"symbol info");
        Check(correct.Definitions.Length==1 && correct.Definitions[0].Line==4,"source definition");
        Check(correct.Highlights!.Any(h=>h.Kind=="type") && correct.Highlights!.Any(h=>h.Kind=="method") && correct.Highlights!.Any(h=>h.Kind=="field") && correct.Highlights!.Any(h=>h.Kind=="parameter"),"semantic colors for types, methods, fields and parameters");
        string colored="using System.Numerics; public class Colors { public Vector3 Feet { get; set; } public bool Fits(float height) => height > 0; public void Resize() { var s = \"🎯\"; Feet = Vector3.Zero; Fits(s.Length); } }";
        var coloring=Analyze(Query(colored,0));
        var bytes=Encoding.UTF8.GetBytes(colored);
        string Slice(Highlight h)=>Encoding.UTF8.GetString(bytes,h.Start,h.Length);
        Check(coloring.Highlights!.Any(h=>h.Kind=="type" && Slice(h)=="Vector3") && coloring.Highlights!.Any(h=>h.Kind=="method" && Slice(h)=="Fits") && coloring.Highlights!.Any(h=>h.Kind=="method" && Slice(h)=="Resize") && coloring.Highlights!.Any(h=>h.Kind=="property" && Slice(h)=="Feet"),"authored methods, metadata types and properties colored by their symbols");
        Check(coloring.Highlights!.Any(h=>h.Kind=="property" && Slice(h)=="Length") && coloring.Highlights!.Any(h=>h.Kind=="local" && Slice(h)=="s"),"UTF-8 highlight positions after a surrogate pair");
        var reference=Analyze(Query(source,source.IndexOf("Speed +=",StringComparison.Ordinal)+3,"references"));
        Check(reference.References.Length==3,"cross-file bound references");
        var renamed=Analyze(Query(source,source.IndexOf("Speed +=",StringComparison.Ordinal)+3,"rename","Velocity"));
        Check(renamed.Edits.Length==3 && renamed.Edits.All(e=>e.Text=="Velocity"),"cross-file rename preview");
        Check(renamed.Edits.Any(e=>e.Path==other && e.Before.EndsWith("\r\n",StringComparison.Ordinal)),"rename preserves closed-file CRLF offsets");
        var invalid=Analyze(Query(source,source.IndexOf("Speed +=",StringComparison.Ordinal)+3,"rename","class"));
        Check(invalid.Error.Length>0 && invalid.Edits.Length==0,"reject keyword rename");
        string partial=source.Replace("Engine.Position(Entity)","Engine.Pos");
        var completion=Analyze(Query(partial,partial.IndexOf("Engine.Pos",StringComparison.Ordinal)+10,"complete"));
        Check(completion.Completions.Any(c=>c.Name=="Position"),"engine member completion");
        Check(completion.Completions.All(c=>c.Name.StartsWith("Pos",StringComparison.OrdinalIgnoreCase)),"member prefix filter");
        var error=Analyze(Query(source.Replace("Speed += dt","MissingName += dt"),0));
        Check(error.Diagnostics.Any(p=>p.Code=="CS0103" && p.Location?.Line==5),"diagnostic location on unsaved buffer");
        var formatted=Analyze(Query(source,0,"format"));
        Check(formatted.Formatted!=null && formatted.Formatted.Contains("// Unicode: 🎯",StringComparison.Ordinal),"format keeps comments and Unicode");
        var malformed=Analyze(Query(source+"public class {",0,"format"));
        Check(malformed.Formatted==null,"refuse format on broken syntax");
        var metadata=Analyze(Query(source,source.IndexOf("Position(Entity)",StringComparison.Ordinal)+3,"rename","Move"));
        Check(metadata.Error.Length>0 && metadata.Edits.Length==0,"cannot rename engine metadata");
        string editorFile=Path.Combine(directory,"assets","Editor","Tool.cs");Directory.CreateDirectory(Path.GetDirectoryName(editorFile)!);
        string editorSource="using Tartarus.Editor; public class Tool : EditorWindow { public override void OnGUI(){ EditorGUILayout.Label(\"Hello\"); } }";
        File.WriteAllText(editorFile,editorSource);
        var editorResult=Analyze(new(directory,editorFile,runtime,[],1,1));
        Check(!editorResult.Diagnostics.Any(p=>p.Severity=="Error"),"editor assembly source partition");
        Console.WriteLine($"IDE Roslyn: {checks} checks, {failures} failure(s)");return failures;
    }
}
