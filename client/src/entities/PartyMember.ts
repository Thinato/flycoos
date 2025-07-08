import * as PIXI from "pixi.js"
import { Vector2 } from "../krol"

export class PartyMember extends PIXI.Container {
  public strength: number = 0
  public vitality: number = 0
  public maxHealth: number = 100
  public health: number = 100
  // public position: Vector2 = new Vector2(0, 0)
  public direction: Vector2 = new Vector2(0, 0)
  public state: "idle" | "run" | "attack" = "idle"
  public sprites: Record<string, PIXI.AnimatedSprite> = {}
  public anchor: { x: number; y: number } = { x: 0.5, y: 0.55 }
  public currentSprite: PIXI.AnimatedSprite | undefined
  public speed: number = 2

  private targetPosition: Vector2 | undefined

  constructor(props: Partial<PartyMember>) {
    super()

    Object.assign(this, props)

    for (const sprite of Object.values(this.sprites)) {
      sprite.anchor.set(this.anchor.x, this.anchor.y)
    }

    console.log("currentAnimation", this.CurrentAnimation)

    this.currentSprite = this.sprites[this.CurrentAnimation]

    console.log("sprites", this.sprites)
    this.currentSprite!.animationSpeed = 0.1

    this.addChild(this.currentSprite)
  }

  update(ticker: PIXI.Ticker) {
    if (this.targetPosition !== undefined) {
      const distanceToTarget = Vector2.fromPoint(this.position)
        .sub(this.targetPosition)
        .magnitude()

      // If we're close enough to the target, stop moving
      if (distanceToTarget < 4) {
        // playerDirection.x = 0
        // playerDirection.y = 0
        this.position.set(this.x, this.y)
        this.targetPosition = undefined
        this.state = "idle"
      } else {
        this.state = "run"
        this.direction.normalize()
        this.position.x += this.direction.x * this.speed * ticker.deltaTime
        this.position.y += this.direction.y * this.speed * ticker.deltaTime
        // console.log("position", this.direction)
        // console.log("position", this.position)
      }
    }

    this.currentSprite?.update(ticker)
    this.currentSprite?.play()
  }

  goTo(position: Vector2) {
    this.state = "run"
    this.targetPosition = position
    this.direction = Vector2.fromPoint(this.position)
      .sub(Vector2.fromPoint(position))
      .invert()
    console.log("currentAnimation", this.CurrentAnimation)
    this.currentSprite!.textures = this.sprites[this.CurrentAnimation].textures
    console.log("currentSprite", this.currentSprite)
    this.currentSprite?.gotoAndPlay(0)
  }

  get CurrentAnimation(): string {
    const angle = this.direction.AngleInDegrees
    console.log("angle", angle)
    if (angle > 135) {
      return this.state + "_w"
    } else if (angle > Math.PI / 4 && angle < Math.PI / 2) {
      return this.state + "_ne"
    } else if (angle > Math.PI / 2 && angle < (3 * Math.PI) / 4) {
      return this.state + "_n"
    } else if (angle > (3 * Math.PI) / 4 && angle < Math.PI) {
      return this.state + "_nw"
    }

    return this.state + "_e"

    // if (this.state === "idle") {
    //   return "idle_" + this.direction.x + "_" + this.direction.y
    // } else if (this.state === "run") {
    //   return "run_" + this.direction.x + "_" + this.direction.y
    // } else if (this.state === "attack") {
    //   return "attack_" + this.direction.x + "_" + this.direction.y
    // }
  }
}
