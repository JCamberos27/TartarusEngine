using System.Numerics;
using Tartarus;
namespace Tartarus.Tests;

public enum ProbeMode { Idle = 0, Moving = 7 }
public sealed class Probe : MonoBehaviour
{
    public string Prefix = "A";
    [Range(0, 10)] public float Speed = 2;
    public ProbeMode Mode = ProbeMode.Moving;
    public Vector3 Offset = Vector3.UnitY;
    [SerializeField, Tooltip("Private serialized value")] float gain = 1.5f;
    [SerializeField, HideInInspector] int updates;
    Animator Animator => GetComponent<Animator>() ?? throw new InvalidOperationException("Missing animator");
    void Count(string name) => Animator.SetInteger(Prefix + name, Animator.GetInteger(Prefix + name) + 1);
    public override void Awake() { Count("Awake"); }
    public override void OnEnable() { Count("Enable"); }
    public override void Start()
    {
        Count("Start");
        Animator.SetInteger(Prefix + "Peers", GetComponents<Probe>().Length);
        Animator.SetBool(Prefix + "Transform", GetComponent<Transform>() != null);
        Animator.SetFloat(Prefix + "Mass", GetComponent<Rigidbody>()?.mass ?? -1);
    }
    public override void Update()
    {
        ++updates; Count("Update"); Animator.SetInteger(Prefix + "State", updates);
        transform.localPosition += Offset * Speed * Tartarus.Time.deltaTime;
        transform.position += Vector3.UnitX * gain * Tartarus.Time.deltaTime;
        NativeRequest request = default;
        if (GetComponent<Rigidbody>() is { } body && Engine.Call(1, ref request) != 0) {
            body.velocity = Vector3.UnitZ * Speed;
            Animator.SetFloat(Prefix + "Velocity", body.velocity.Z);
            body.AddForce(Vector3.UnitZ, ForceMode.Impulse);
            Debug.Assert(Physics.Raycast(new(0,0,-5),Vector3.UnitZ,out var ray,10) && ray.Object.Id==Entity);
            Debug.Assert(Physics.SphereCast(new(0,0,-5),.2f,Vector3.UnitZ,out var sphere,10) && sphere.Object.Id==Entity);
            Debug.Assert(Physics.OverlapSphere(Vector3.Zero,2).Any(o=>o.Id==Entity));
            Animator.SetBool("ApiQueries",true);
        }
    }
    public override void FixedUpdate() => Count("Fixed");
    public override void LateUpdate() => Count("Late");
    public override void OnDisable() => Count("Disable");
    public override void OnDestroy() => Count("Destroy");
}
